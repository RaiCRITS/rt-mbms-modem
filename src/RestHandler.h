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

#pragma once
#include <string>
#include <vector>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <libconfig.h++>

#include "SdrReader.h"
#include "Phy.h"

#include "cpprest/json.h"
#include "cpprest/http_listener.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "cpprest/filestream.h"
#include "cpprest/containerstream.h"
#include "cpprest/producerconsumerstream.h"

const int CINR_RAVG_CNT = 100;
typedef enum { searching, syncing, processing } state_t;

/**
 *  The RESTful API handler. Supports GET and PUT verbs for SDR parameters, and GET for reception info
 */
class RestHandler {
  public:
    /**
     *  Definition of the callback for setting new reception parameters
     */
    typedef std::function<void(const std::string& antenna, unsigned fcen, double gain, unsigned sample_rate, unsigned bandwidth)> set_params_t;

    /**
     *  Definition of the callback for switching to a named frequency-scan preset.
     *  Returns false if the preset name isn't recognised.
     */
    typedef std::function<bool(const std::string& mode)> set_scan_mode_t;

    /**
     *  Definition of the callback for persisting the ce_enable flag (channel estimate
     *  weighting in MBSFN soft demodulation) to the config file. Takes effect at the
     *  next modem restart. Returns false if the config file could not be written.
     */
    typedef std::function<bool(bool enabled)> set_ce_enable_t;

    /**
     *  Default constructor.
     *
     *  @param cfg Config singleton reference
     *  @param url URL to open the server on
     *  @param state Reference to the main loop sate
     *  @param sdr Reference to the SDR reader
     *  @param set_params Set parameters callback
     *  @param set_scan_mode Set scan mode preset callback
     */
    RestHandler(const libconfig::Config& cfg, const std::string& url, std::atomic<state_t>& state,
        SdrReader& sdr, Phy& phy, set_params_t set_params, set_scan_mode_t set_scan_mode,
        set_ce_enable_t set_ce_enable);
    /**
     *  Default destructor.
     */
    virtual ~RestHandler();

    /**
     *  RX Info pertaining to an SCH (MCCH/MCH or PDSCH)
     */
    class ChannelInfo {
      public:
        void SetData( std::vector<uint8_t> data) {
          std::lock_guard<std::mutex> lock(_data_mutex);
          _data = data;
        };
        std::vector<uint8_t> GetData() { 
          std::lock_guard<std::mutex> lock(_data_mutex);
          return _data; 
        };
        bool present = false;
        int mcs = 0;
        double ber = 0;      // no longer measured since the srsLTE -> srsRAN rebase, kept for API compatibility
        float evm = 0;       // PDSCH only: PMCH does not measure EVM
        float avg_iterations = 0;
        unsigned total = 1;
        unsigned errors = 0;
      private:
        std::vector<uint8_t> _data = {};
        std::mutex _data_mutex;
    };

    /**
     *  Time domain subcarrier CE values. Guarded by a mutex: written by the CAS
     *  worker thread, read by the REST threads.
     */
    void set_ce_values(std::vector<uint8_t> v) {
      std::lock_guard<std::mutex> lock(_ce_values_mutex);
      _ce_values = std::move(v);
    }
    std::vector<uint8_t> get_ce_values() {
      std::lock_guard<std::mutex> lock(_ce_values_mutex);
      return _ce_values;
    }

    /**
     *  RX info for PDSCH
     */
    ChannelInfo _pdsch;

    /**
     *  RX info for MCCH
     */
    ChannelInfo _mcch;

    /**
     *  RX info for MCHs. Dimensione fissa (max 15 PMCH per area MBSFN, TS 36.331):
     *  niente inserimenti dinamici, quindi worker e thread REST non possono
     *  corrompere la struttura del container accedendovi in parallelo.
     */
    std::array<ChannelInfo, 16> _mch;

    /**
     *  Current CINR value on the CAS
     */
    float cinr_db() { return _cinr_db.load(); }
    void add_cinr_value( float cinr);

    /**
     *  Current CINR value on the MBSFN channel. Measured separately: the CAS
     *  can be perfectly readable while the MBSFN carrier is not.
     */
    float cinr_mbsfn_db() { return _cinr_mbsfn_db.load(); }
    void add_cinr_mbsfn_value( float cinr);

    /**
     *  Processing time of the last MBSFN frame (microseconds)
     */
    std::atomic<uint32_t> mbsfn_frame_time_us{0};

    /**
     *  Processing time of the last CAS frame (microseconds)
     */
    std::atomic<uint32_t> cas_frame_time_us{0};

    /**
     *  ce_enable value currently active in the MBSFN processors (config + CLI override),
     *  set once at startup and exposed on GET /status. A differing value written via
     *  PUT /ce_enable becomes active only after a restart.
     */
    bool _ce_enabled_active = false;

  private:
    // Exponential moving averages, written by the CAS/MBSFN worker threads and
    // read from the main and REST threads: atomic, so no container to protect.
    std::atomic<float> _cinr_db{0};
    std::atomic<float> _cinr_mbsfn_db{0};

    void get(web::http::http_request message);
    void put(web::http::http_request message);
    void options(const web::http::http_request& message);

    web::json::value get_system_status();
    uint64_t _prev_cpu_total = 0;
    uint64_t _prev_cpu_idle = 0;

    // Replies with an Access-Control-Allow-Origin header so the REST API can be called
    // directly from a web app served on a different origin/port (e.g. a local kiosk UI).
    static void reply_cors(const web::http::http_request& message, web::http::status_code status);
    static void reply_cors(const web::http::http_request& message, web::http::status_code status, const web::json::value& body);
    static void reply_cors(const web::http::http_request& message, web::http::status_code status,
        const Concurrency::streams::istream& body, const utility::string_t& content_type = U("application/octet-stream"));

    std::unique_ptr<web::http::experimental::listener::http_listener> _listener;

    std::vector<uint8_t> _ce_values = {};
    std::mutex _ce_values_mutex;

    std::atomic<state_t>& _state;
    SdrReader& _sdr;
    Phy& _phy;

    set_params_t _set_params;
    set_scan_mode_t _set_scan_mode;
    set_ce_enable_t _set_ce_enable;

    bool _require_bearer_token = false;
    std::string _api_key;
};

