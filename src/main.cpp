// 5G-MAG Reference Tools
// MBMS Modem Process
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

/**
 * @file main.cpp
 * @brief Contains the program entry point, command line parameter handling, and the main runloop for data processing.
 */

/** \mainpage 5G-MAG Reference Tools - MBMS Modem
 *
 * This is the documentation for the FeMBMS receiver. Please see main.cpp for for the runloop and main processing logic as a starting point.
 *
 */

#include <argp.h>

#include <chrono>
#include <cstdlib>
#include <libconfig.h++>

#include "CasFrameProcessor.h"
#include "Gw.h"
#include "SdrReader.h"
#include "MbsfnFrameProcessor.h"
#include "MeasurementFileWriter.h"
#include "Phy.h"
#include "RestHandler.h"
#include "Rrc.h"
#include "Version.h"
#include "spdlog/async.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/syslog_sink.h"
#include "srsran/srsran.h"
#include "srsran/upper/pdcp.h"
#include "srsran/rlc/rlc.h"
#include "thread_pool.hpp"

 #include "dview.hpp" //ALC add loggher


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

using namespace std::placeholders;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "5G-MAG Reference Tools CRITS<reference-tools@5g-mag.com>";
static char doc[] = "5G-MAG-RT MBMS Modem Process CRITS!";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"config", 'c', "FILE", 0, "Configuration file (default: /etc/5gmag-rt.conf)", 0},
    {"log-level", 'l', "LEVEL", 0,
     "Log verbosity: 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error, 5 = "
     "critical, 6 = none. Default: 2.",
     0},
    {"srsran-log-level", 's', "LEVEL", 0,
     "Log verbosity for srsran: 0 = debug, 1 = info, 2 = warn, 3 = error, 4 = "
     "none, Default: 4.",
     0},
    {"sample-file", 'f', "FILE", 0,
     "Sample file in 4 byte float interleaved format to read I/Q data from. If "
     "present, the data from this file will be decoded instead of live SDR "
     "data. The channel bandwith must be specified with the --file-bandwidth "
     "flag, and the sample rate of the file must be suitable for this "
     "bandwidth.",
     0},
    {"write-sample-file", 'w', "FILE", 0,
     "Create a sample file in 4 byte float interleaved format containing the "
     "raw received I/Q data.",
     0},
    {"file-bandwidth", 'b', "BANDWIDTH (MHz)", 0,
     "If decoding data from a file, specify the channel bandwidth of the "
     "recorded data in MHz here (e.g. 5)",
     0},
    {"override_nof_prb", 'p', "# PRB", 0,
     "Override the number of PRB received in the MIB", 0},
    {"sdr_devices", 'd', nullptr, 0,
     "Prints a list of all available SDR devices", 0},
    {"scan", 'S', "MODE", 0,
     "Scan mode preset: 'TV8' (freq=610MHz, step=8MHz), 'TV6' (freq=605MHz, step=6MHz), 'TV7' (freq=613.5MHz, step=7MHz), or 'LTE' (freq=609.5MHz, step=5MHz). When set, overrides config file frequency_step_hz and number_of_step.",
     0},
    {"ce", 'e', "0|1", 0,
     "Enable (1) or disable (0) channel estimate weighting in MBSFN soft demodulation. Overrides "
     "modem.phy.ce_enable from the config file. Default (if neither is set): disabled.",
     0},
    {nullptr, 0, nullptr, 0, nullptr, 0}};

/**
 * Holds all options passed on the command line
 */
struct arguments {
  const char *config_file = {};  /**< file path of the config file. */
  unsigned log_level = 2;        /**< log level */
  unsigned srs_log_level = 4;    /**< srsLTE log level */
  int8_t override_nof_prb = -1;  /**< ovride PRB number */
  const char *sample_file = {};  /**< file path of the sample file. */
  uint8_t file_bw = 0;           /**< bandwidth of the sample file */
  const char
      *write_sample_file = {};   /**< file path of the created sample file. */
  bool list_sdr_devices = false;
  const char *scan_mode = {};    /**< scan preset mode: "Europe", "America", or "LTE" */
  int8_t ce_enabled = -1;        /**< CLI override for modem.phy.ce_enable: -1 = not passed, 0/1 = force disabled/enabled */
};

/**
 * Parses the command line options into the arguments struct.
 */
static auto parse_opt(int key, char *arg, struct argp_state *state) -> error_t {
  auto arguments = static_cast<struct arguments *>(state->input);
  switch (key) {
    case 'c':
      arguments->config_file = arg;
      break;
    case 'l':
      arguments->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case 's':
      arguments->srs_log_level =
          static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case 'f':
      arguments->sample_file = arg;
      break;
    case 'w':
      arguments->write_sample_file = arg;
      break;
    case 'b':
      arguments->file_bw = static_cast<uint8_t>(strtoul(arg, nullptr, 10));
      break;
    case 'p':
      arguments->override_nof_prb =
          static_cast<int8_t>(strtol(arg, nullptr, 10));
      break;
    case 'd':
      arguments->list_sdr_devices = true;
      break;
    case 'S':
      arguments->scan_mode = arg;
      break;
    case 'e':
      arguments->ce_enabled = (strtoul(arg, nullptr, 10) != 0) ? 1 : 0;
      break;
    case ARGP_KEY_ARG:
      argp_usage(state);
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc,
                           nullptr, nullptr,   nullptr};

/**
 * Print the program version in MAJOR.MINOR.PATCH format.
 */
void print_version(FILE *stream, struct argp_state * /*state*/) {
  fprintf(stream, "%s.%s.%s \n", std::to_string(VERSION_MAJOR).c_str(),
          std::to_string(VERSION_MINOR).c_str(),
          std::to_string(VERSION_PATCH).c_str());
}

static Config cfg;  /**< Global configuration object. */

static unsigned sample_rate = 7680000;  /**< Sample rate of the SDR */
static unsigned search_sample_rate = 7680000;  /**< Sample rate of the SDR */
static unsigned frequency = 667000000;  /**< Center freqeuncy the SDR is tuned to */
static uint32_t bandwidth = 10000000;   /**< Low pass filter bandwidth for the SDR */
static double gain = 0.9;               /**< Overall system gain for the SDR */
static std::string antenna = "LNAW";    /**< Antenna input to be used */
static bool use_agc = false;

/* Frequency-step scanning around the configured/preset frequency. Global (not local to
 * main()) so the RESTful API can also change scan mode at runtime, not just at startup. */
static unsigned frequency_step = 1000000;
static unsigned number_of_step = 0;
static unsigned start_frequency = frequency;  /**< Frequency to return to when a scan restarts */
static unsigned step = 0;                     /**< Current frequency-step index of the scan */

/**
 * One named frequency-scan preset (e.g. "TV8"): starting frequency, step size, and step count.
 */
struct ScanPreset {
  unsigned freq;
  unsigned step_hz;
  unsigned n_steps;
  const char* label;
};

/**
 * Looks up a scan mode preset by name. Shared by CLI startup parsing and the
 * RESTful API's runtime scan-mode switch, so the preset table only exists once.
 *
 * @param mode Preset name: "TV8", "TV6", "TV7", or "LTE"
 * @param out Filled with the preset's parameters on success
 * @return true if mode was recognised
 */
static bool lookup_scan_preset(const std::string& mode, ScanPreset& out) {
  if (mode == "TV8") {
    out = {610000000, 8000000, 10, "TV8"};
  } else if (mode == "TV6") {
    out = {605000000, 6000000, 15, "TV6"};
  } else if (mode == "TV7") {
    out = {613500000, 7000000, 17, "TV7"};
  } else if (mode == "LTE") {
    out = {609500000, 5000000, 17, "LTE"};
  } else {
    return false;
  }
  return true;
}

static unsigned mbsfn_nof_prb = 0;
static unsigned cas_nof_prb = 0;

/**
 * Restart flag. Setting this to true triggers resynchronization using the params set in the following parameters:
 * @see sample_rate
 * @see frequency
 * @see bandwith
 * @see gain
 * @see antenna
 */
static bool restart = false;

/**
 * Set new SDR parameters and initialize resynchronisation. This function is used by the RESTful API handler
 * to modify the SDR params.
 *
 * @param ant  Name of the antenna input (For LimeSDR Mini: LNAW, LNAL)
 * @param fc   Center frequency to tune to (in Hz)
 * @param gain Total system gain to set [0..1]
 * @param sr   Sample rate (in Hz)
 * @param bw   Low pass filter bandwidth (in Hz)
 */
void set_params(const std::string& ant, unsigned fc, double g, unsigned sr, unsigned bw) { //NOLINT
  sample_rate = sr;
  frequency = fc;
  bandwidth = bw;
  antenna = ant;
  gain = g;
  spdlog::info("RESTful API requesting new parameters: fc {}, bw {}, rate {}, gain {}, antenna {}",
      frequency, bandwidth, sample_rate, gain, antenna);

  restart = true;
}

/**
 * Switch to a named frequency-scan preset and (re)start the search from its beginning.
 * Used by the RESTful API handler to let the WUI trigger a scan without a restart.
 *
 * @param mode Preset name: "TV8", "TV6", "TV7", or "LTE"
 * @return true if mode was recognised and the scan was (re)started
 */
bool set_scan_mode(const std::string& mode) { //NOLINT
  ScanPreset preset;
  if (!lookup_scan_preset(mode, preset)) {
    spdlog::warn("RESTful API requested unknown scan mode: {}", mode);
    return false;
  }
  frequency = preset.freq;
  frequency_step = preset.step_hz;
  number_of_step = preset.n_steps;
  start_frequency = frequency;
  step = 0;
  restart = true;
  spdlog::info("RESTful API requesting scan mode {}: frequency={} MHz, frequency_step={} MHz x{}",
      preset.label, frequency / 1e6, frequency_step / 1e6, number_of_step);
  return true;
}

/**
 * Writes the discovered center frequency to the configuration file.
 * 
 * @param config_file Path to the configuration file
 * @param freq The frequency to write (in Hz)
 */
void write_frequency_to_config(const char* config_file, unsigned freq) {
  try {
    Config cfg;
    cfg.readFile(config_file);
    
    // Get the modem.sdr setting group
    libconfig::Setting& root = cfg.getRoot();
    libconfig::Setting& modem = root["modem"];
    libconfig::Setting& sdr = modem["sdr"];
    
    // Try to find and update existing center_frequency_hz setting
    try {
      sdr.lookup("center_frequency_hz") = (long long)freq;
    } catch(const libconfig::SettingNotFoundException &ex) {
      // If it doesn't exist, add it
      sdr.add("center_frequency_hz", libconfig::Setting::TypeInt64) = (long long)freq;
    }
    
    cfg.writeFile(config_file);
    
    spdlog::info("Written discovered frequency {} Hz ({} MHz) to config file {}", 
        freq, freq / 1000000.0, config_file);
  } catch(const FileIOException &fioex) {
    spdlog::warn("I/O error while writing config file: {}. Frequency not saved.", config_file);
  } catch(const ParseException &pex) {
    spdlog::warn("Config parse error while updating frequency: {}:{} - {}", 
        pex.getFile(), pex.getLine(), pex.getError());
  } catch(const std::exception &ex) {
    spdlog::warn("Error while writing frequency to config: {}", ex.what());
  }
}

/**
 *  Main entry point for the program.
 *  
 * @param argc  Command line agument count
 * @param argv  Command line arguments
 * @return 0 on clean exit, -1 on failure
 */
auto main(int argc, char **argv) -> int {
  spdlog::info("nthread-{}", std::thread::hardware_concurrency());
  try {
  struct arguments arguments;
  /* Default values */
  arguments.config_file = "/etc/5gmag-rt.conf";
  arguments.sample_file = nullptr;
  arguments.write_sample_file = nullptr;
  argp_parse(&argp, argc, argv, 0, nullptr, &arguments);

  // Read and parse the configuration file
  try {
    cfg.readFile(arguments.config_file);
  } catch(const FileIOException &fioex) {
    spdlog::error("I/O error while reading config file at {}. Exiting.", arguments.config_file);
    exit(1);
  } catch(const ParseException &pex) {
    spdlog::error("Config parse error at {}:{} - {}. Exiting.",
        pex.getFile(), pex.getLine(), pex.getError());
    exit(1);
  }

  // Set up logging
  std::string ident = "modem";
  auto syslog_logger = spdlog::syslog_logger_mt("syslog", ident, LOG_PID | LOG_PERROR | LOG_CONS );

  spdlog::set_level(
      static_cast<spdlog::level::level_enum>(arguments.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f %z] [%^%l%$] [thr %t] %v");

  spdlog::set_default_logger(syslog_logger);
  spdlog::info("5g-mag-rt modem v{}.{}.{} starting up", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

  // Init and tune the SDR
  auto rx_channels = 1;
  cfg.lookupValue("modem.sdr.rx_channels", rx_channels);
  spdlog::info("Initialising SDR with {} RX channel(s)", rx_channels);
  SdrReader sdr(cfg, rx_channels);
  if (arguments.list_sdr_devices) {
    sdr.enumerateDevices();
    exit(0);
  }

  std::string sdr_dev = "driver=lime";
  cfg.lookupValue("modem.sdr.device_args", sdr_dev);
  if (!sdr.init(sdr_dev, arguments.sample_file, arguments.write_sample_file)) {
    spdlog::error("Failed to initialize I/Q data source.");
    exit(1);
  }

  
  if (!cfg.lookupValue("modem.sdr.search_sample_rate_hz", search_sample_rate)) {
    search_sample_rate = 7680000;  // Default search sample rate
    spdlog::warn("modem.sdr.search_sample_rate_hz not found in config. Using default: 7680000 Hz");
  }
  sample_rate = search_sample_rate;

  if (!cfg.lookupValue("modem.sdr.filter_bandwidth_hz", bandwidth)) {
    bandwidth = 10000000;  // Default LPF bandwidth (10 MHz)
    spdlog::warn("modem.sdr.filter_bandwidth_hz not found in config. Using default: 10000000 Hz");
  }

  unsigned long long center_frequency = frequency;
  if (!cfg.lookupValue("modem.sdr.center_frequency_hz", center_frequency)) {
    spdlog::error("Unable to parse center_frequency_hz - values must have a ‘L’ character appended");
    exit(1);
  }
  // We needed unsigned long long for correct parsing,
  // but unsigned is required
  if (center_frequency <= UINT_MAX) {
     frequency = static_cast<unsigned>(center_frequency);
  } else {
    spdlog::error("Configured center_frequency_hz is {}, maximal value supported is {}.",
        center_frequency, UINT_MAX);
    exit(1);
  }

  // Check if scan mode is specified
  if (arguments.scan_mode != nullptr) {
    ScanPreset preset;
    if (!lookup_scan_preset(arguments.scan_mode, preset)) {
      spdlog::error("Unknown scan mode: {}. Valid modes are: TV8, TV6, TV7, LTE", arguments.scan_mode);
      exit(1);
    }
    frequency = preset.freq;
    frequency_step = preset.step_hz;
    number_of_step = preset.n_steps;
    spdlog::info("Using {} scan mode: frequency={} MHz, frequency_step={} MHz",
        preset.label, frequency / 1e6, frequency_step / 1e6);
  }

  // Only read from config file if scan mode is not being used
  /*
  if (!use_scan_mode) {
    int tmp_step_int = 0;
    int tmp_nsteps_int = 0;
    if (cfg.lookupValue("modem.sdr.frequency_step_hz", tmp_step_int)) {
      if (tmp_step_int > 0) frequency_step = static_cast<unsigned>(tmp_step_int);
      spdlog::info("Loaded frequency_step_hz from config: {}", frequency_step);
    } else {
      spdlog::warn("modem.sdr.frequency_step_hz not found in config. Using default: 1000000 Hz");
    }
    if (cfg.lookupValue("modem.sdr.number_of_step", tmp_nsteps_int)) {
      if (tmp_nsteps_int > 0) number_of_step = static_cast<unsigned>(tmp_nsteps_int);
      spdlog::info("Loaded number_of_step from config: {}", number_of_step);
    } else {
      spdlog::warn("modem.sdr.number_of_step not found in config. Using default: 1");
    }
      
  }*/
  /* --- ALC: Optional frequency stepping for scanning around the configured frequency. */

  cfg.lookupValue("modem.sdr.normalized_gain", gain);
  cfg.lookupValue("modem.sdr.antenna", antenna);
  cfg.lookupValue("modem.sdr.use_agc", use_agc);

  
  if (!sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc)) {
    spdlog::error("Failed to set initial center frequency. Exiting.");
    exit(1);
  }

  srslog::set_default_sink( srslog::fetch_stdout_sink() );
  set_srsran_verbose_level(arguments.log_level <= 1 ? SRSRAN_VERBOSE_DEBUG : SRSRAN_VERBOSE_NONE);
  srsran_use_standard_symbol_size(true);

  // Create a thread pool for the frame processors
  unsigned thread_cnt = 4;
  cfg.lookupValue("modem.phy.threads", thread_cnt);
  int phy_prio = 10;
  cfg.lookupValue("modem.phy.thread_priority_rt", phy_prio);
  thread_pool pool{ thread_cnt + 1, phy_prio };

  // Elevate execution to real time scheduling
  struct sched_param thread_param = {};
  thread_param.sched_priority = 20;
  cfg.lookupValue("modem.phy.main_thread_priority_rt", thread_param.sched_priority);

  spdlog::info("Raising main thread to realtime scheduling priority {}", thread_param.sched_priority);

  int error = pthread_setschedparam(pthread_self(), SCHED_RR, &thread_param);
  if (error != 0) {
    spdlog::error("Cannot set main thread priority to realtime: {}. Thread will run at default priority.", strerror(error));
  }

  bool enable_measurement_file = false;
  cfg.lookupValue("modem.measurement_file.enabled", enable_measurement_file);
  MeasurementFileWriter measurement_file(cfg);

  auto srs_level = srslog::basic_levels::none;
  switch (arguments.srs_log_level) {
    case 0: srs_level = srslog::basic_levels::debug; break;
    case 1: srs_level = srslog::basic_levels::info; break;
    case 2: srs_level = srslog::basic_levels::warning; break;
    case 3: srs_level = srslog::basic_levels::error; break;
    case 4: srs_level = srslog::basic_levels::none; break;
    default: break;
  }

  // Configure srsLTE logging
 srslog::init();
 srslog::fetch_basic_logger("ALL").set_level(srs_level);
 auto& mac_log = srslog::fetch_basic_logger("MAC");
  mac_log.set_level(srs_level);
 auto& phy_log = srslog::fetch_basic_logger("PHY");
  phy_log.set_level(srs_level);
 auto& rlc_log = srslog::fetch_basic_logger("RLC");
  rlc_log.set_level(srs_level);
 auto& asn1_log = srslog::fetch_basic_logger("ASN1");
  asn1_log.set_level(srs_level);

  // Create the layer components: Phy, RLC, RRC and GW
  Phy phy(
      cfg,
      std::bind(&SdrReader::get_samples, &sdr, _1, _2, _3),  // NOLINT
      arguments.file_bw ? arguments.file_bw * 5 : 25,  //ALC if non reading from file, assume 25 PRB (5MHz @ 15kHz)
      arguments.override_nof_prb,
      rx_channels);

  phy.init();

  srsran::pdcp pdcp(nullptr, "PDCP");
  srsran::rlc rlc("RLC");
  srsran::timer_handler timers;

  Rrc rrc(cfg, phy, rlc);
  Gw gw(cfg, phy);
  gw.init();

  rlc.init(&pdcp, &rrc, &timers, 0 /* RB_ID_SRB0 */);
  pdcp.init(&rlc, &rrc,  &gw);



  state_t state = searching;

  // Create the RESTful API handler
  std::string uri = "http://0.0.0.0:3010/modem-api/";
  cfg.lookupValue("modem.restful_api.uri", uri);
  spdlog::info("Starting RESTful API handler at {}", uri);
  RestHandler rest_handler(cfg, uri, state, sdr, phy, set_params, set_scan_mode);

  // Initialize one CAS and thread_cnt MBSFN frame processors
  CasFrameProcessor cas_processor(cfg, phy, rlc, rest_handler, rx_channels);
  if (!cas_processor.init()) {
    spdlog::error("Failed to create CAS processor. Exiting.");
    exit(1);
  }

  std::vector<MbsfnFrameProcessor*> mbsfn_processors;
  for (auto i = 0U; i < thread_cnt; i++) {
    auto p = new MbsfnFrameProcessor(cfg, rlc, phy, mac_log, rest_handler, rx_channels, arguments.ce_enabled);
    if (!p->init()) {
      spdlog::error("Failed to create MBSFN processor. Exiting.");
      exit(1);
    }
    mbsfn_processors.push_back(p);
  }

  // Start receiving sample data
   sdr.start();

  uint32_t tti = 0;

  uint32_t measurement_interval = 5;
  cfg.lookupValue("modem.measurement_file.interval_secs", measurement_interval);
  measurement_interval *= 1000;
  uint32_t tick = 0;

  // Initial state: searching a cell
  state = searching;

  // Start the main processing loop
  start_frequency = frequency;  // Remember the frequency for restart scan -  ALC
  step = 0;  // number of step for search frequency - ALC
  unsigned sync_fail_count = 0;   // consecutive MIB sync failures in syncing state - ALC
  unsigned search_fail_count = 0; // consecutive cell_search() failures in searching state - ALC
  unsigned max_sync_fails = 5;    // max failures before forcing a frequency scan restart - ALC
  cfg.lookupValue("modem.phy.max_sync_fails", max_sync_fails);
  for (;;) {
    if (state == searching) {
      if (restart) {
        sdr.stop();
        sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc);
        sdr.start();
      }

      // We're at the search sample rate, and there's no point in creating a sample file. Stop the sample writer, if enabled.
      sdr.disableSampleFileWriting();

      // In searching state, clear the receive buffer and try to find a cell at the configured frequency and synchronize with it
      restart = false;
      spdlog::info("Clear buffer and start cell search at frequency {} MHz with sample rate {} MHz", frequency / 1e6, sample_rate / 1e6); //ALC addel LOG
      sdr.clear_buffer();
      // TODO: Re-enable cell_search_adv once wideband scanner buffer allocation is fixed
      bool cell_found = phy.cell_search();  // Fallback to stable cell_search for now
      if (cell_found) {
        search_fail_count = 0;  // reset on success - ALC
        // A cell has been found. We now know the required number of PRB = bandwidth of the carrier. Set the approproiate
        // sample rate...
        spdlog::info("Cell found at frequency {} MHz", frequency / 1e6);
        ok proviamo
        // Write the discovered frequency to the configuration file
        write_frequency_to_config(arguments.config_file, frequency);
        
        cas_nof_prb = mbsfn_nof_prb = phy.nr_prb();

        if (arguments.sample_file && arguments.file_bw) {
          // Samples files are recorded at a fixed sample rate that can be determined from the bandwidth command line argument.
          // If we're decoding from file, do not readjust the rate to match the CAS PRBs, but stay at this rate and instead configure the
          // PHY to decode a narrow CAS from a wider channel.
          mbsfn_nof_prb = arguments.file_bw * 5;
          phy.set_nof_mbsfn_prb(mbsfn_nof_prb);
          phy.set_cell();
        } else {
          // When decoding from the air, configure the SDR accordingly
          unsigned new_srate = srsran_sampling_freq_hz(cas_nof_prb);
          if (new_srate != sample_rate) {
            sample_rate = new_srate;
            spdlog::info("Setting sample rate {} Mhz for {} PRB / {} Mhz channel width", new_srate/1000000.0, phy.nr_prb(),
                phy.nr_prb() * 0.2);
            sdr.stop();

            bandwidth = (cas_nof_prb * 200000);
            sdr.tune(frequency, new_srate, bandwidth, gain, antenna, use_agc);

            sdr.start();
          }
        }
        spdlog::debug("Synchronizing subframe");
        // ... and move to syncing state.
        state = syncing;
      } else {
        // cell_search() failed — check for persistent overflow/MIB failure and force SDR hard reset - ALC
        search_fail_count++;
        if (search_fail_count >= max_sync_fails) {
          spdlog::warn("cell_search failed {} times in a row — forcing SDR hard reset to recover from buffer overflow.", search_fail_count);
          search_fail_count = 0;
          sdr.stop();
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc);
          sdr.start();
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        //frequency-step search loop added — try `number_of_step` frequencies spaced by `frequency_step` - ALC
        if (step < number_of_step) {
          step++;
          frequency = start_frequency + step * frequency_step;
          spdlog::info("Trying frequency {} MHz (step {}/{})", frequency / 1000000.0, step + 1, number_of_step);
          // (re)tune SDR to the candidate frequency for this step
          sdr.stop();
          sdr.clear_buffer();
          if (!sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc)) {
            spdlog::warn("Tuning to {} Hz failed; continuing to next step.", frequency);
            sdr.start();
            continue;
          }
          sdr.start();
        } else {
          if (step == number_of_step && number_of_step > 0) {  //if the frequency scan is active (number_of_step > 0) but max number of step is reached
            spdlog::info("Nothing found during the scan, restart at frequency {} MHz)", start_frequency / 1000000.0);
            frequency = start_frequency; //return to original frequency - ALC
            sample_rate = search_sample_rate;  // sample rate for searching
          } 
          sleep(1);
        }
        //-- frequency-step search loop added — try `number_of_step` frequencies spaced by `frequency_step`
      }
    } else if (state == syncing) {
      // In syncing state, we already know the cell we want to camp on, and the SDR is tuned to the required
      // sample rate for its number of PRB / bandwidth. We now synchronize PSS/SSS and receive the MIB once again
      // at this sample rate.
      unsigned max_frames = 200;
      bool sfn_sync = false;
      while (!sfn_sync && max_frames-- > 0) {
        sfn_sync = phy.synchronize_subframe();
      }

      if (max_frames == 0 && !sfn_sync) {
        sync_fail_count++;
        spdlog::warn("Synchronization failed ({}/{}). Going back to search state.", sync_fail_count, max_sync_fails);
        if (sync_fail_count >= max_sync_fails) {
          spdlog::warn("Too many consecutive sync failures — forcing frequency scan restart.");
          sync_fail_count = 0;
          step = 0;
          frequency = start_frequency;
          sample_rate = search_sample_rate;
          sdr.stop();
          sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc);
          sdr.start();
        }
        state = searching;
        sleep(1);
      }

      if (sfn_sync) {
        // We're locked on to the cell, and have succesfully received the MIB at the target sample rate.
        sync_fail_count = 0;  // reset failure counter on successful sync - ALC
        spdlog::info("Decoded MIB at target sample rate, TTI is {}. Subframe synchronized.", phy.tti());

        // Set the cell parameters in the CAS processor
        cas_processor.set_cell(phy.cell());

        for (auto i = 0U; i < thread_cnt; i++) {
          mbsfn_processors[i]->unlock();
        }

        // Get the initial TTI / subframe ID (= system frame number * 10 + subframe number)
        tti = phy.tti();
        // Reset the RRC
        rrc.reset();

        // Ready to receive actual data. Go to processing state.
        state = processing;
//        dvw::start();

        // If sample file creation is enabled, start writing out samples now that we're at the target sample rate
        sdr.enableSampleFileWriting();
      }
    } else {  // processing
      int mb_idx = 0;
      while (state == processing) {
        tti = (tti + 1) % 10240; // Clamp the TTI
//        dvw::log_i("tti", tti); //ALC log the TTI for debugging purposes
        if (phy.is_cas_subframe(tti)) {
          // Get the samples from the SDR interface, hand them to a CAS processor, and start it
          // on a thread from the pool.
          if (!restart && phy.get_next_frame(cas_processor.rx_buffer(), cas_processor.rx_buffer_size())) {
            spdlog::debug("sending tti {} to regular processor", tti); 
            pool.push([ObjectPtr = &cas_processor, tti, &rest_handler] {  // ALC è una lambda che cattura un puntatore alla cas_processor e lo invia a un thread 
                if (ObjectPtr->process(tti)) {
                // Set constellation diagram data and rx params for CAS in the REST API handler
                rest_handler.add_cinr_value(ObjectPtr->cinr_db());
                }
                });


            if (phy.nof_mbsfn_prb() != mbsfn_nof_prb)
            {
              // Handle the non-LTE bandwidths (6, 7 and 8 MHz). In these cases, CAS stays at the original bandwidth, but the MBSFN
              // portion of the frames can be wider. We need to...

              mbsfn_nof_prb = phy.nof_mbsfn_prb();

              // ...adjust the SDR's sample rate to fit the wider MBSFN bandwidth...
              unsigned new_srate = srsran_sampling_freq_hz(mbsfn_nof_prb);
              bandwidth = (mbsfn_nof_prb * 200000);
              // ... configure the PHY and CAS processor to decode a narrow CAS and wider MBSFN, and move back to syncing state
              // after reconfiguring and restarting the SDR.
              phy.set_cell();
              cas_processor.set_cell(phy.cell());
              if (new_srate != sample_rate) {
                spdlog::info("Setting sample rate {} Mhz for MBSFN with {} PRB / {} Mhz channel width", new_srate/1000000.0, mbsfn_nof_prb,
                    mbsfn_nof_prb * 0.2);
                sdr.stop();
                sdr.tune(frequency, new_srate, bandwidth, gain, antenna, use_agc);
                sdr.start();
              }
              spdlog::info("Synchronizing subframe after PRB extension");
              state = syncing;
            }
          } else {
            // Failed to receive data, or sync lost. Go back to searching state.
            sdr.stop();
            sample_rate = search_sample_rate;  // sample rate for searching
            sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc);
            sdr.start();
            rrc.reset();
            phy.reset();

            sleep(1);
            state = searching;
          }
        } else {
          // All other frames in FeMBMS dedicated mode are MBSFN frames.
          spdlog::debug("sending tti {} to mbsfn proc {}", tti, mb_idx);
          // Get the samples from the SDR interface, hand them to an MNSFN processor, and start it
          // on a thread from the pool. Getting the buffer pointer from the pool also locks this processor.
          if (!restart && phy.get_next_frame(mbsfn_processors[mb_idx]->get_rx_buffer_and_lock(), mbsfn_processors[mb_idx]->rx_buffer_size())) {
            if (phy.mcch_configured() && phy.is_mbsfn_subframe(tti)) {
              // If data frm SIB1/SIB13 has been received in CAS, configure the processors accordingly
              if (!mbsfn_processors[mb_idx]->mbsfn_configured()) {
                srsran_scs_t scs = SRSRAN_SCS_15KHZ;
                switch (phy.mbsfn_subcarrier_spacing()) {
                  case Phy::SubcarrierSpacing::df_15kHz:  scs = SRSRAN_SCS_15KHZ; break;
                  case Phy::SubcarrierSpacing::df_7kHz5:  scs = SRSRAN_SCS_7KHZ5; break;
                  case Phy::SubcarrierSpacing::df_2kHz5:  scs = SRSRAN_SCS_2KHZ5; break;
                  case Phy::SubcarrierSpacing::df_1kHz25: scs = SRSRAN_SCS_1KHZ25; break;
                  case Phy::SubcarrierSpacing::df_0kHz37: scs = SRSRAN_SCS_0KHZ37; break;
                }
                auto cell = phy.cell();
                cell.nof_prb = cell.mbsfn_prb;
                mbsfn_processors[mb_idx]->set_cell(cell);
                mbsfn_processors[mb_idx]->configure_mbsfn(phy.mbsfn_area_id(), scs);
              }
              pool.push([ObjectPtr = mbsfn_processors[mb_idx], tti] {  // ALC assegna gli MBSFN  al thread pool  
                ObjectPtr->process(tti);
              });
            } else {
              // Nothing to do yet, we lack the data from SIB1/SIB13
              // Discard the samples and unlock the processor.
              mbsfn_processors[mb_idx]->unlock();
            }
          } else {
            // Failed to receive data, or sync lost. Go back to searching state.
            spdlog::warn("Synchronization lost while processing. Going back to searching state.");
            sdr.stop();
            sample_rate = search_sample_rate;  // sample rate for searching
            sdr.tune(frequency, sample_rate, bandwidth, gain, antenna, use_agc);
            sdr.start();

            state = searching;
            sleep(1);
            rrc.reset();
            phy.reset();
          }
          mb_idx = static_cast<int>((mb_idx + 1) % thread_cnt);
        }

        tick++;
        if (tick%measurement_interval == 0) {
          // It's time to output rx info to the measurement file and to syslog.
          // Collect the relevant info and write it out.
          std::vector<std::string> cols;

          spdlog::info("CINR {:.2f} dB", rest_handler.cinr_db() );
          cols.push_back(std::to_string(rest_handler.cinr_db()));

          spdlog::info("PDSCH: MCS {}, BLER {}, BER {}",
              rest_handler._pdsch.mcs,
              ((rest_handler._pdsch.errors * 1.0) / (rest_handler._pdsch.total * 1.0)),
              rest_handler._pdsch.ber);
          cols.push_back(std::to_string(rest_handler._pdsch.mcs));
          cols.push_back(std::to_string(((rest_handler._pdsch.errors * 1.0) / (rest_handler._pdsch.total * 1.0))));
          cols.push_back(std::to_string(rest_handler._pdsch.ber));

          spdlog::info("MCCH: MCS {}, BLER {}, BER {}",
              rest_handler._mcch.mcs,
              ((rest_handler._mcch.errors * 1.0) / (rest_handler._mcch.total * 1.0)),
              rest_handler._mcch.ber);

          cols.push_back(std::to_string(rest_handler._mcch.mcs));
          cols.push_back(std::to_string(((rest_handler._mcch.errors * 1.0) / (rest_handler._mcch.total * 1.0))));
          cols.push_back(std::to_string(rest_handler._mcch.ber));

          auto mch_info = phy.mch_info();
          int mch_idx = 0;
          std::for_each(std::begin(mch_info), std::end(mch_info), [&cols, &mch_idx, &rest_handler](Phy::mch_info_t const& mch) {
              spdlog::info("MCH {}: MCS {}, BLER {}, BER {}",
                  mch_idx,
                  mch.mcs,
                  (rest_handler._mch[mch_idx].errors * 1.0) / (rest_handler._mch[mch_idx].total * 1.0),
                  rest_handler._mch[mch_idx].ber);
              cols.push_back(std::to_string(mch_idx));
              cols.push_back(std::to_string(mch.mcs));
              cols.push_back(std::to_string((rest_handler._mch[mch_idx].errors * 1.0) / (rest_handler._mch[mch_idx].total * 1.0)));
              cols.push_back(std::to_string(rest_handler._mch[mch_idx].ber));

              int mtch_idx = 0;
              std::for_each(std::begin(mch.mtchs), std::end(mch.mtchs), [&mtch_idx](Phy::mtch_info_t const& mtch) {
                spdlog::info("    MTCH {}: LCID {}, TMGI 0x{}, {}",
                  mtch_idx,
                  mtch.lcid,
                  mtch.tmgi,
                  mtch.dest);
                mtch_idx++;
                  });
                mch_idx++;
              });
          spdlog::info("-----");
          if (enable_measurement_file) {
            measurement_file.WriteLogValues(cols);
          }
        }
      }
//      dvw::stop();
    }
  }

  // Main loop ended by signal. Free the MBSFN processors, and bail.
  for (auto i = 0U; i < thread_cnt; i++) {
    delete( mbsfn_processors[i] );
  }
  } catch (...) {
    exit(1);
  }
  return 0;
}
