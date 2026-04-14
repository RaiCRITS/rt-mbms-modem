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

#include "CasFrameProcessor.h"
#include "spdlog/spdlog.h"
#include <cmath>


auto CasFrameProcessor::init() -> bool {
  _signal_buffer_max_samples = 3 * SRSRAN_SF_LEN_PRB(MAX_PRB);

  for (auto ch = 0U; ch < _rx_channels; ch++) {
    _signal_buffer_rx[ch] = srsran_vec_cf_malloc(_signal_buffer_max_samples);
    if (!_signal_buffer_rx[ch]) {
      spdlog::error("Could not allocate regular DL signal buffer\n");
      return false;
    }
  }

  if (srsran_ue_dl_init(&_ue_dl, _signal_buffer_rx, MAX_PRB, _rx_channels)) {
    spdlog::error("Could not init ue_dl\n");
    return false;;
  }

  srsran_softbuffer_rx_init(&_softbuffer, 100);

  _ue_dl_cfg.snr_to_cqi_offset = 0;

  for (auto & i : _data) {
    i = srsran_vec_u8_malloc(2000 * 8);
    if (!i) {
      spdlog::error("Allocating data");
      return false;
    }
  }

  srsran_chest_dl_cfg_t* chest_cfg = &_ue_dl_cfg.chest_cfg;
  memset(chest_cfg, 0, sizeof(srsran_chest_dl_cfg_t));
  chest_cfg->filter_coef[0] = 4;
  chest_cfg->filter_coef[1] = 1.0f;
  chest_cfg->filter_type = SRSRAN_CHEST_FILTER_GAUSS;
  chest_cfg->noise_alg = SRSRAN_NOISE_ALG_EMPTY;
  chest_cfg->rsrp_neighbour       = false;
  chest_cfg->sync_error_enable    = false;
  chest_cfg->estimator_alg = SRSRAN_ESTIMATOR_ALG_AVERAGE;
  chest_cfg->cfo_estimate_enable  = true;
  chest_cfg->cfo_estimate_sf_mask = 1023;

  _ue_dl_cfg.cfg.pdsch.csi_enable         = true;
  _ue_dl_cfg.cfg.pdsch.max_nof_iterations = 8;
  _ue_dl_cfg.cfg.pdsch.meas_evm_en        = false;
  _ue_dl_cfg.cfg.pdsch.decoder_type       = SRSRAN_MIMO_DECODER_MMSE;
  _ue_dl_cfg.cfg.pdsch.softbuffers.rx[0] = &_softbuffer;

  return true;
}

CasFrameProcessor::~CasFrameProcessor() {
  for (auto & i : _data) {
    if (i) {
      free(i);
    }
  }
  srsran_softbuffer_rx_free(&_softbuffer);
  srsran_ue_dl_free(&_ue_dl);
}

void CasFrameProcessor::set_cell(srsran_cell_t cell) {
  _cell = cell;
  spdlog::info("CAS processor setting cell ({} PRB / {} MBSFN PRB), cp={}.",
               cell.nof_prb, cell.mbsfn_prb, SRSRAN_CP_ISNORM(cell.cp) ? "NORM" : "EXT");
  srsran_ue_dl_set_cell(&_ue_dl, cell);
}

auto CasFrameProcessor::process(uint32_t tti) -> bool {
  _sf_cfg.tti = tti;
  _sf_cfg.cfi = _cell.semi_static_cfi ? _cell.semi_static_cfi : 0;
  _sf_cfg.sf_type = SRSRAN_SF_NORM;

  if ((tti/10)%100 == 0) {
    _rest._pdsch.total = 0;
    _rest._pdsch.errors = 0;
  }

  _rest._pdsch.total++;

  // Run the FFT and do channel estimation
  if (srsran_ue_dl_decode_fft_estimate(&_ue_dl, &_sf_cfg, &_ue_dl_cfg) < 0) {
    _rest._pdsch.errors++;
    spdlog::error("Getting PDCCH FFT estimate\n");
    _mutex.unlock();
    return false;
  }

  // Feedback the CFO from CE to the Phy
  _phy.set_cfo_from_channel_estimation(_ue_dl.chest_res.cfo);

  // Try to decode DCIs from PDCCH
  srsran_dci_dl_t dci[SRSRAN_MAX_CARRIERS] = {};    // NOLINT
  int nof_grants = srsran_ue_dl_find_dl_dci(&_ue_dl, &_sf_cfg, &_ue_dl_cfg, _cell.mbms_dedicated ? SRSRAN_SIRNTI_MBMS_DEDICATED : SRSRAN_SIRNTI, dci);

  // DIAG: log every CAS subframe so we can track when grants appear
  spdlog::info("[CAS DIAG] tti={} nof_grants={} cell(id={},prb={},ports={},mbms_ded={},pbch_rep_r16={},cfi={})",
               tti, nof_grants,
               _cell.id, _cell.nof_prb, _cell.nof_ports,
               _cell.mbms_dedicated, _cell.has_pbch_repetition_r16,
               _sf_cfg.cfi);

  for (int k = 0; k < nof_grants; k++) {
    char str[512];  // NOLINT
    srsran_dci_dl_info(&dci[k], str, 512);
    _rest._pdsch.mcs = static_cast<int>(dci[k].tb[0].mcs_idx);
    spdlog::info("[CAS DIAG] DCI[{}]: {} snr={:.1f}dB rnti=0x{:04x} n_prb1a={}", k, str, _ue_dl.chest_res.snr_db, dci[k].rnti, dci[k].type2_alloc.n_prb1a);

    if (srsran_ue_dl_dci_to_pdsch_grant(&_ue_dl, &_sf_cfg, &_ue_dl_cfg, &dci[k], &_ue_dl_cfg.cfg.pdsch.grant)) {
      spdlog::error("Converting DCI message to DL dci\n");
    _mutex.unlock();
      return false;
    }

    // We can construct a DL grant
    _ue_dl_cfg.cfg.pdsch.rnti = dci[k].rnti;
    srsran_pdsch_cfg_t* pdsch_cfg = &_ue_dl_cfg.cfg.pdsch;

    srsran_pdsch_res_t pdsch_res[SRSRAN_MAX_CODEWORDS] = {};  // NOLINT
    for (int i = 0; i < SRSRAN_MAX_CODEWORDS; i++) {
      if (pdsch_cfg->grant.tb[i].enabled) {
        if (pdsch_cfg->grant.tb[i].rv < 0) {
          uint32_t sfn              = tti / 10;
          uint32_t k                = (sfn / 2) % 4;
          pdsch_cfg->grant.tb[i].rv = ((int32_t)ceilf(1.5F * static_cast<float>(k))) % 4;
        }
        pdsch_res[i].payload = _data[i];
        pdsch_res[i].crc     = false;
        srsran_softbuffer_rx_reset_tbs(pdsch_cfg->softbuffers.rx[i], (uint32_t)pdsch_cfg->grant.tb[i].tbs);
      }
    }

    // DIAG: log PDSCH grant details before decode
    {
      std::string prb_map0, prb_map1;
      for (int n = 0; n < (int)_cell.nof_prb; n++) {
        prb_map0 += pdsch_cfg->grant.prb_idx[0][n] ? "1" : "0";
        prb_map1 += pdsch_cfg->grant.prb_idx[1][n] ? "1" : "0";
      }
      spdlog::info("[CAS DIAG] PDSCH grant: nof_prb={} nof_re={} tb0(tbs={},rv={},mod={}) nof_bits={}",
                   pdsch_cfg->grant.nof_prb, pdsch_cfg->grant.nof_re,
                   pdsch_cfg->grant.tb[0].tbs, pdsch_cfg->grant.tb[0].rv,
                   (int)pdsch_cfg->grant.tb[0].mod, pdsch_cfg->grant.tb[0].nof_bits);
      spdlog::info("[CAS DIAG] prb_slot0={}", prb_map0);
      spdlog::info("[CAS DIAG] prb_slot1={}", prb_map1);
    }

    if (_vis_data_interval > 0 && (++_vis_data_counter % _vis_data_interval == 0)) { //ALC: Only send visualization data to the REST API every _vis_data_interval subframes
        _rest._pdsch.SetData(pdsch_data());
        _rest._ce_values = ce_values();
    }

    // Decode PDSCH with the DCI RNTI (0xFFF9 for FeMBMS SI)
    auto ret = srsran_ue_dl_decode_pdsch(&_ue_dl, &_sf_cfg, &_ue_dl_cfg.cfg.pdsch, pdsch_res);
    if (ret) {
      spdlog::error("Error decoding PDSCH\n");
      _rest._pdsch.errors++;
    } else {
      spdlog::debug("Decoded PDSCH");
      for (int i = 0; i < SRSRAN_MAX_CODEWORDS; i++) {
        if (pdsch_cfg->grant.tb[i].enabled) {
          spdlog::info("PDSCH TB[{}]: tbs={}, rv={}, crc={} (rnti=0x{:04x})", i,
                        pdsch_cfg->grant.tb[i].tbs,
                        pdsch_cfg->grant.tb[i].rv,
                        pdsch_res[i].crc ? "OK" : "FAIL",
                        pdsch_cfg->rnti);
          // Log first 16 decoded bytes regardless of CRC (to check consistency)
          {
            char hexbuf[64] = {};
            int nbytes = std::min((uint32_t)16, (uint32_t)(pdsch_cfg->grant.tb[i].tbs / 8));
            for (int b = 0; b < nbytes; b++) {
              snprintf(hexbuf + b*3, 4, "%02x ", _data[i][b]);
            }
            spdlog::info("[CAS DIAG] TB[{}] first {} bytes: {}", i, nbytes, hexbuf);
          }
          // .. and pass received PDUs to RLC for further processing
          if (pdsch_res[i].crc) {
            _rlc.write_pdu_bcch_dlsch(_data[i], (uint32_t)pdsch_cfg->grant.tb[i].tbs);
          }
        }
      }
    }

    // Log equalized QPSK symbols to check constellation orientation.
    // For correct QPSK: each symbol should be near (±0.7, ±0.7). Mean abs ~0.7.
    // If phase-rotated by 90°: (Re,Im) will be swapped/negated.
    {
      const cf_t* syms = _ue_dl.pdsch.d[0];
      float sum_abs = 0;
      int ncheck = std::min(32, (int)pdsch_cfg->grant.nof_re);
      for (int s = 0; s < ncheck; s++) {
        float re = __real__ syms[s], im = __imag__ syms[s];
        sum_abs += sqrtf(re*re + im*im);
      }
      spdlog::info("[CAS DIAG] QPSK mean_abs={:.3f}", sum_abs / ncheck);
      // Print first 8 symbols as (Re,Im) pairs to check constellation orientation
      char sym_buf[256] = {};
      int pos = 0;
      for (int s = 0; s < 8 && s < (int)pdsch_cfg->grant.nof_re; s++) {
        float re = __real__ syms[s], im = __imag__ syms[s];
        pos += snprintf(sym_buf+pos, sizeof(sym_buf)-pos, "(%.2f,%.2f) ", re, im);
      }
      spdlog::info("[CAS DIAG] first 8 QPSK syms: {}", sym_buf);
    }

    // BRUTE-FORCE RNTI SCAN: run once to find the correct PDSCH scrambling RNTI.
    // We re-demodulate d[0] (equalized symbols, untouched by scrambling) to get
    // true raw LLRs, then test every RNTI via turbo CRC.
    static bool rnti_scan_done = false;
    if (!rnti_scan_done && pdsch_cfg->grant.tb[0].nof_bits > 0) {
      rnti_scan_done = true;
      spdlog::info("[RNTI SCAN] Starting (tti={}, rv={}, tbs={}, nof_bits={}, cell_id={})",
                   _sf_cfg.tti, pdsch_cfg->grant.tb[0].rv,
                   pdsch_cfg->grant.tb[0].tbs,
                   pdsch_cfg->grant.tb[0].nof_bits, _cell.id);

      const uint32_t nof_bits = pdsch_cfg->grant.tb[0].nof_bits;  // 4284
      const uint32_t nslot    = 2 * (_sf_cfg.tti % 10);           // 0 for CAS subframe
      const uint32_t tbs      = pdsch_cfg->grant.tb[0].tbs;       // 256

      // Temporary RX softbuffer for scan
      srsran_softbuffer_rx_t scan_sb = {};
      srsran_softbuffer_rx_init(&scan_sb, 100);
      srsran_softbuffer_rx_t* orig_sb = pdsch_cfg->softbuffers.rx[0];
      pdsch_cfg->softbuffers.rx[0]    = &scan_sb;

      uint32_t orig_max_iter = _ue_dl.pdsch.dl_sch.max_iterations;
      _ue_dl.pdsch.dl_sch.max_iterations = 16;

      std::vector<int16_t> test_e(nof_bits);
      std::vector<uint8_t> scan_data(tbs / 8 + 8, 0);

      // -----------------------------------------------------------------------
      // LOOPBACK TEST: verify encode→decode chain works end-to-end
      // -----------------------------------------------------------------------
      {
        srsran_softbuffer_tx_t tx_sb = {};
        srsran_softbuffer_tx_init(&tx_sb, 100);
        srsran_softbuffer_tx_reset_tbs(&tx_sb, tbs);
        srsran_softbuffer_tx_t* orig_tx = pdsch_cfg->softbuffers.tx[0];
        pdsch_cfg->softbuffers.tx[0]    = &tx_sb;

        // Known payload: 32 bytes recognizable pattern
        std::vector<uint8_t> lb_data(tbs / 8, 0);
        for (size_t i = 0; i < lb_data.size(); i++) lb_data[i] = (uint8_t)(i + 1);

        // Encode: output is PACKED bits, ceil(nof_bits/8) bytes
        std::vector<uint8_t> lb_packed(nof_bits / 8 + 1, 0);
        int enc_ret = srsran_dlsch_encode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                           lb_data.data(), lb_packed.data(), 0, 1);
        pdsch_cfg->softbuffers.tx[0] = orig_tx;
        srsran_softbuffer_tx_free(&tx_sb);

        if (enc_ret != SRSRAN_SUCCESS) {
          spdlog::error("[LOOPBACK] encode failed: {}", enc_ret);
        } else {
          // Unpack PACKED bits → int16 LLRs (srsRAN convention: negative=bit0, positive=bit1)
          std::vector<int16_t> lb_llr(nof_bits);
          for (uint32_t b = 0; b < nof_bits; b++) {
            uint8_t bit = (lb_packed[b / 8] >> (7 - (b % 8))) & 1;
            lb_llr[b]   = bit ? +100 : -100;  // negative=0, positive=1 (srsRAN convention)
          }

          // Test 1: decode without scrambling (should pass CRC)
          srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
          std::fill(scan_data.begin(), scan_data.end(), 0);
          int dec_ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                             lb_llr.data(), scan_data.data(), 0, 1);
          spdlog::info("[LOOPBACK] encode→decode (no scramble): {}  first4={:02x}{:02x}{:02x}{:02x}",
                       dec_ret == SRSRAN_SUCCESS ? "CRC=OK" : "CRC=FAIL",
                       scan_data[0], scan_data[1], scan_data[2], scan_data[3]);

          // Test 2: scramble with 0xFFF9 then descramble → should still pass
          std::vector<int16_t> lb_scr(nof_bits);
          srsran_sequence_pdsch_apply_s(lb_llr.data(), lb_scr.data(),
                                        0xFFF9, 0, nslot, _cell.id, nof_bits);
          std::vector<int16_t> lb_descr(nof_bits);
          srsran_sequence_pdsch_apply_s(lb_scr.data(), lb_descr.data(),
                                        0xFFF9, 0, nslot, _cell.id, nof_bits);
          srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
          std::fill(scan_data.begin(), scan_data.end(), 0);
          dec_ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                          lb_descr.data(), scan_data.data(), 0, 1);
          spdlog::info("[LOOPBACK] encode→scramble→descramble→decode: {}",
                       dec_ret == SRSRAN_SUCCESS ? "CRC=OK" : "CRC=FAIL");

          // Test 3: try scanning scrambled lb_llr to find RNTI=0xFFF9
          bool lb_scan_found = false;
          for (uint32_t test_rnti = 0xFFF0; test_rnti <= 0xFFFF; test_rnti++) {
            srsran_sequence_pdsch_apply_s(lb_scr.data(), test_e.data(),
                                          (uint16_t)test_rnti, 0, nslot, _cell.id, nof_bits);
            srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
            int ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                            test_e.data(), scan_data.data(), 0, 1);
            if (ret == SRSRAN_SUCCESS) {
              spdlog::info("[LOOPBACK] scan found rnti=0x{:04x} in loopback test (expected 0xFFF9)", test_rnti);
              lb_scan_found = true;
            }
          }
          if (!lb_scan_found)
            spdlog::error("[LOOPBACK] scan DID NOT find 0xFFF9 in loopback! decode infrastructure BROKEN");
        }
      }

      // -----------------------------------------------------------------------
      // RECEIVED SIGNAL: re-demodulate d[0] → raw LLRs, apply 0xFFF9 scrambling
      // -----------------------------------------------------------------------
      std::vector<int16_t> raw_e(nof_bits);
      srsran_demod_soft_demodulate_s(pdsch_cfg->grant.tb[0].mod,
                                     _ue_dl.pdsch.d[0],
                                     raw_e.data(),
                                     (int)pdsch_cfg->grant.nof_re,
                                     nullptr);
      {
        char rbuf[128] = {};
        for (int k = 0; k < 12; k++) snprintf(rbuf + k * 7, 8, "%6d ", raw_e[k]);
        spdlog::info("[RNTI SCAN] raw LLRs (from d[0]): {}", rbuf);

        // Log descrambled LLRs with RNTI=0xFFF9 (first 20 values)
        std::vector<int16_t> dscr_e(nof_bits);
        srsran_sequence_pdsch_apply_s(raw_e.data(), dscr_e.data(),
                                      0xFFF9, 0, nslot, _cell.id, nof_bits);
        char dbuf[160] = {};
        for (int k = 0; k < 20; k++) snprintf(dbuf + k * 7, 8, "%6d ", dscr_e[k]);
        spdlog::info("[RNTI SCAN] descrambled LLRs (rnti=0xFFF9): {}", dbuf);

        // Decode with 0xFFF9 descrambling
        srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
        std::fill(scan_data.begin(), scan_data.end(), 0);
        int ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                        dscr_e.data(), scan_data.data(), 0, 1);
        spdlog::info("[RNTI SCAN] decode with rnti=0xFFF9: {}  first4={:02x}{:02x}{:02x}{:02x}",
                     ret == SRSRAN_SUCCESS ? "CRC=OK" : "CRC=FAIL",
                     scan_data[0], scan_data[1], scan_data[2], scan_data[3]);
      }

      // No-scramble decode
      {
        srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
        int ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                       raw_e.data(), scan_data.data(), 0, 1);
        spdlog::info("[RNTI SCAN] no-scramble decode: {}", ret == SRSRAN_SUCCESS ? "CRC=OK" : "CRC=FAIL");
      }

      // -----------------------------------------------------------------------
      // FULL SCAN: all 65536 RNTIs × cell_id
      // -----------------------------------------------------------------------
      for (uint32_t scan_cell_id : {_cell.id, _cell.id + 1u, _cell.id + 2u}) {
        bool found_this_cell = false;
        for (uint32_t test_rnti = 0; test_rnti <= 0xFFFF && !found_this_cell; test_rnti++) {
          srsran_sequence_pdsch_apply_s(raw_e.data(), test_e.data(),
                                        (uint16_t)test_rnti, 0, nslot, scan_cell_id, nof_bits);
          srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
          int ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                         test_e.data(), scan_data.data(), 0, 1);
          if (ret == SRSRAN_SUCCESS) {
            spdlog::info("[RNTI SCAN] FOUND rnti=0x{:04x} cell_id={} CRC=OK  first4={:02x}{:02x}{:02x}{:02x}",
                         test_rnti, scan_cell_id,
                         scan_data[0], scan_data[1], scan_data[2], scan_data[3]);
            found_this_cell = true;
          }
          if (test_rnti % 8192 == 0)
            spdlog::info("[RNTI SCAN] cell_id={} progress: {}/65536", scan_cell_id, test_rnti);
        }
        spdlog::info("[RNTI SCAN] cell_id={} done: found={}", scan_cell_id, found_this_cell ? "YES" : "NO");
      }

      // -----------------------------------------------------------------------
      // RE-SCAN with standard LTE RE exclusion (mbms_dedicated=false → E=4284)
      // Tests whether the original PSS/SSS+PBCH exclusion gives decodable bits.
      // -----------------------------------------------------------------------
      {
        bool saved_mbms_ded        = _cell.mbms_dedicated;
        bool saved_ue_dl_mbms_ded  = _ue_dl.pdsch.cell.mbms_dedicated;
        _cell.mbms_dedicated              = false;
        _ue_dl.pdsch.cell.mbms_dedicated  = false;

        uint32_t saved_nof_re   = pdsch_cfg->grant.nof_re;
        uint32_t saved_nof_bits = (uint32_t)pdsch_cfg->grant.tb[0].nof_bits;

        uint32_t std_nof_re   = srsran_ra_dl_grant_nof_re(&_cell, &_sf_cfg, &pdsch_cfg->grant);
        uint32_t std_nof_bits = std_nof_re * 2;  // QPSK
        pdsch_cfg->grant.nof_re          = std_nof_re;
        pdsch_cfg->grant.tb[0].nof_bits  = (int)std_nof_bits;

        spdlog::info("[E_SCAN] std_LTE re-extract: nof_re={} nof_bits={}", std_nof_re, std_nof_bits);

        // Re-extract symbols from resource grid with standard LTE exclusions
        srsran_pdsch_res_t std_res[SRSRAN_MAX_CODEWORDS] = {};
        std_res[0].payload = scan_data.data();
        srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
        srsran_ue_dl_decode_pdsch(&_ue_dl, &_sf_cfg, &_ue_dl_cfg.cfg.pdsch, std_res);
        spdlog::info("[E_SCAN] std_LTE direct decode with rnti=0x{:04x}: crc={}",
                     pdsch_cfg->rnti, std_res[0].crc ? "OK" : "FAIL");

        // Re-demodulate and try full RNTI scan
        std::vector<int16_t> std_raw(std_nof_bits);
        srsran_demod_soft_demodulate_s(pdsch_cfg->grant.tb[0].mod,
                                       _ue_dl.pdsch.d[0], std_raw.data(),
                                       std_nof_re, nullptr);
        {
          char rbuf[128] = {};
          for (int k = 0; k < 12 && k < (int)std_nof_bits; k++)
            snprintf(rbuf + k*7, 8, "%6d ", std_raw[k]);
          spdlog::info("[E_SCAN] std_LTE raw LLRs (first 12): {}", rbuf);
        }

        std::vector<int16_t> std_e(std_nof_bits);
        bool std_found = false;
        for (uint32_t scan_cell_id : {_cell.id + 333u, _cell.id + 334u, _cell.id + 335u}) {
          // Note: _cell.id is now 333 (unchanged), we pass actual cell IDs
          (void)scan_cell_id;
        }
        for (uint32_t scan_cell_id : {_cell.id, 333u}) {
          for (uint32_t test_rnti = 0; test_rnti <= 0xFFFF && !std_found; test_rnti++) {
            srsran_sequence_pdsch_apply_s(std_raw.data(), std_e.data(),
                                          (uint16_t)test_rnti, 0, nslot, scan_cell_id, std_nof_bits);
            srsran_softbuffer_rx_reset_tbs(&scan_sb, tbs);
            int ret = srsran_dlsch_decode2(&_ue_dl.pdsch.dl_sch, pdsch_cfg,
                                            std_e.data(), scan_data.data(), 0, 1);
            if (ret == SRSRAN_SUCCESS) {
              spdlog::info("[E_SCAN] FOUND rnti=0x{:04x} cell_id={} E={} CRC=OK  first4={:02x}{:02x}{:02x}{:02x}",
                           test_rnti, scan_cell_id, std_nof_bits,
                           scan_data[0], scan_data[1], scan_data[2], scan_data[3]);
              std_found = true;
            }
            if (test_rnti % 8192 == 0)
              spdlog::info("[E_SCAN] cell_id={} E={} progress: {}/65536", scan_cell_id, std_nof_bits, test_rnti);
          }
          spdlog::info("[E_SCAN] cell_id={} E={} done: found={}", scan_cell_id, std_nof_bits, std_found ? "YES" : "NO");
        }

        // Restore cell state
        _cell.mbms_dedicated             = saved_mbms_ded;
        _ue_dl.pdsch.cell.mbms_dedicated = saved_ue_dl_mbms_ded;
        pdsch_cfg->grant.nof_re          = saved_nof_re;
        pdsch_cfg->grant.tb[0].nof_bits  = (int)saved_nof_bits;
      }

      // Restore state
      _ue_dl.pdsch.dl_sch.max_iterations = orig_max_iter;
      pdsch_cfg->softbuffers.rx[0]       = orig_sb;
      srsran_softbuffer_rx_free(&scan_sb);
      srsran_softbuffer_rx_reset_tbs(pdsch_cfg->softbuffers.rx[0], tbs);
    }
  }
    _mutex.unlock();
  return true;
}

auto CasFrameProcessor::ce_values() -> std::vector<uint8_t> {
  auto sz = (uint32_t)srsran_symbol_sz(_cell.nof_prb);
  std::vector<float> ce_abs;
  ce_abs.resize(sz, 0);
  uint32_t g = (sz - 12 * _cell.nof_prb) / 2;
  srsran_vec_abs_dB_cf(_ue_dl.chest_res.ce[0][0], -80, &ce_abs[g], SRSRAN_NRE * _cell.nof_prb);
  const uint8_t* data = reinterpret_cast<uint8_t*>(ce_abs.data());
  return { data, data + sz * sizeof(float)};
}

auto CasFrameProcessor::pdsch_data() -> std::vector<uint8_t> {
  const uint8_t* data = reinterpret_cast<uint8_t*>(_ue_dl.pdsch.d[0]);
  return { data, data + _ue_dl_cfg.cfg.pdsch.grant.nof_re * sizeof(cf_t)};
}
