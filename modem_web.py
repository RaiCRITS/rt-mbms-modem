#!/usr/bin/env python3
"""
ALC
MBMS Modem Web Monitor
Avvia un mini server HTTP che aggrega i dati del modem e li espone
come JSON a una pagina HTML con aggiornamento automatico.

Dipendenze: pip install requests
Uso: python3 modem_web.py [http://modem-host:porta/modem-api] [--port 8080]

Aprire nel browser: http://localhost:8080
"""

import sys
import json
import time
import threading
import argparse
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse
import requests

DEFAULT_MODEM = "http://127.0.0.1:3010/modem-api"
DEFAULT_PORT  = 8080

# ── HTML della pagina (servita come stringa embedded) ─────────────────────────

HTML = """<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MBMS Monitor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: #1a1a1a;
    color: #ccc;
    font-family: 'Courier New', monospace;
    font-size: 14px;
    padding: 16px;
  }

  /* ── Header ── */
  #header {
    display: flex;
    align-items: center;
    gap: 24px;
    border-bottom: 1px solid #444;
    padding-bottom: 10px;
    margin-bottom: 14px;
  }
  #header h1 { font-size: 16px; color: #fff; letter-spacing: 1px; }
  #state { font-weight: bold; font-size: 15px; }
  #freq  { color: #fff; font-weight: bold; }
  #last-update { margin-left: auto; font-size: 11px; color: #555; }

  /* ── Pannelli ── */
  #panels {
    display: flex;
    gap: 14px;
    align-items: flex-start;
  }

  .panel {
    border: 1px solid #444;
    border-radius: 4px;
    padding: 10px 14px;
    flex: 1;
  }
  .panel h2 {
    font-size: 12px;
    letter-spacing: 1px;
    text-transform: uppercase;
    margin-bottom: 8px;
    padding-bottom: 4px;
    border-bottom: 1px solid #333;
  }
  #panel-sdr   h2 { color: #4fc3f7; border-color: #4fc3f7; }
  #panel-chan  h2 { color: #ce93d8; border-color: #ce93d8; }
  #panel-const h2 { color: #a5d6a7; border-color: #a5d6a7; }

  /* ── Costellazione ── */
  #panel-const { flex: 0 0 auto; }
  #const-wrap {
    position: relative;
    display: inline-block;
  }
  #const-canvas {
    display: block;
    background: #111;
    border: 1px solid #333;
  }
  #const-sel {
    margin-top: 6px;
    background: #2a2a2a;
    border: 1px solid #555;
    color: #ccc;
    font-family: inherit;
    font-size: 12px;
    padding: 2px 6px;
    width: 100%;
    border-radius: 3px;
  }

  /* ── Tabella chiave/valore (SDR) ── */
  .kv { width: 100%; border-collapse: collapse; }
  .kv td { padding: 2px 6px; vertical-align: top; }
  .kv td:first-child { color: #888; width: 110px; white-space: nowrap; }
  .kv td:last-child  { color: #ddd; }

  /* ── Tabella canali ── */
  #chan-table { width: 100%; border-collapse: collapse; }
  #chan-table th {
    text-align: left;
    color: #888;
    font-weight: normal;
    padding: 2px 8px;
    border-bottom: 1px solid #333;
  }
  #chan-table th:not(:first-child) { text-align: right; }
  #chan-table td { padding: 3px 8px; }
  #chan-table td:not(:first-child) { text-align: right; color: #aaa; }
  #chan-table tr.absent td { color: #555; }

  /* ── Colori di stato ── */
  .state-searching    { color: #ffc107; }
  .state-syncing      { color: #26c6da; }
  .state-synchronized { color: #66bb6a; }
  .state-unknown      { color: #ef5350; }

  .cinr-good { color: #66bb6a; font-weight: bold; }
  .cinr-warn { color: #ffc107; font-weight: bold; }

  .flag-ok  { color: #66bb6a; }
  .flag-off { color: #555; }

  /* ── Footer ── */
  #footer {
    margin-top: 14px;
    border-top: 1px solid #333;
    padding-top: 8px;
    display: flex;
    align-items: center;
    gap: 16px;
    font-size: 12px;
    color: #666;
  }
  #footer label { color: #aaa; }
  #freq-input {
    background: #2a2a2a;
    border: 1px solid #555;
    color: #fff;
    padding: 3px 8px;
    font-family: inherit;
    font-size: 13px;
    width: 120px;
    border-radius: 3px;
  }
  #freq-btn {
    background: #1565c0;
    color: #fff;
    border: none;
    padding: 4px 12px;
    cursor: pointer;
    font-family: inherit;
    border-radius: 3px;
  }
  #freq-btn:hover    { background: #1976d2; }
  #restart-btn {
    background: #4a148c;
    color: #fff;
    border: none;
    padding: 4px 12px;
    cursor: pointer;
    font-family: inherit;
    border-radius: 3px;
  }
  #restart-btn:hover { background: #6a1b9a; }
  #notice { color: #ffc107; font-weight: bold; }
  #error  { color: #ef5350; }
</style>
</head>
<body>

<div id="header">
  <h1>MBMS Monitor</h1>
  <span id="state">–</span>
  <span id="freq">–</span>
  <span id="last-update"></span>
</div>

<div id="panels">
  <div class="panel" id="panel-sdr">
    <h2>SDR / RF</h2>
    <table class="kv" id="sdr-table"></table>
  </div>
  <div class="panel" id="panel-chan">
    <h2>Canali</h2>
    <table id="chan-table">
      <thead>
        <tr>
          <th>Canale</th>
          <th>MCS</th>
          <th>BER</th>
          <th>BLER</th>
          <th></th>
        </tr>
      </thead>
      <tbody id="chan-body"></tbody>
    </table>
  </div>
  <div class="panel" id="panel-const">
    <h2>Costellazione</h2>
    <div id="const-wrap">
      <canvas id="const-canvas" width="220" height="220"></canvas>
    </div>
    <select id="const-sel" onchange="constChannel = this.value">
      <option value="pdsch">PDSCH</option>
      <option value="mcch">MCCH</option>
    </select>
  </div>
</div>

<div id="footer">
  <label>Frequenza (MHz):</label>
  <input id="freq-input" type="number" step="0.001" placeholder="es. 943.200">
  <button id="freq-btn" onclick="sendFreq()">Imposta</button>
  <button id="restart-btn" onclick="sendRestart()">Restart</button>
  <span id="notice"></span>
  <span id="error"></span>
</div>

<script>
// ── Helpers ──────────────────────────────────────────────────────────────────

function na(v, fmt) {
  return (v !== null && v !== undefined) ? fmt(v) : "N/A";
}
function fmtFreq(hz) { return (hz / 1e6).toFixed(4) + " MHz"; }
function fmtMhz(hz)  { return (hz / 1e6).toFixed(3) + " MHz"; }
function fmtDb(v)    { return v.toFixed(2) + " dB"; }
function fmtHz(v)    { return (v >= 0 ? "+" : "") + v.toFixed(1) + " Hz"; }
function fmtPct(v)   { return (v * 100).toFixed(1) + "%"; }

// ── Aggiornamento UI ─────────────────────────────────────────────────────────

function updateHeader(d) {
  const state = d.status ? d.status.state : null;
  const stateEl = document.getElementById("state");
  stateEl.textContent = state ? "● " + state.toUpperCase() : "● N/A";
  stateEl.className = "state-" + (state || "unknown");

  const freq = d.sdr ? d.sdr.frequency : null;
  document.getElementById("freq").textContent = na(freq, fmtFreq);

  document.getElementById("last-update").textContent =
    "aggiornato " + new Date().toLocaleTimeString();
}

function updateSdr(d) {
  const sdr    = d.sdr    || {};
  const status = d.status || {};
  const cinr   = status.cinr_db;

  const rows = [
    ["Frequenza",   na(sdr.frequency,    fmtFreq), ""],
    ["Gain",        na(sdr.gain,         fmtDb),   ""],
    ["Sample rate", na(sdr.sample_rate,  fmtMhz),  ""],
    ["Filter BW",   na(sdr.filter_bw,   fmtMhz),  ""],
    ["Buffer",      na(sdr.buffer_level, fmtPct),  ""],
    ["Antenna",     sdr.antenna || "N/A",           ""],
    ["CINR",
      na(cinr, fmtDb),
      cinr !== undefined && cinr !== null
        ? (cinr > 10 ? "cinr-good" : "cinr-warn")
        : ""],
    ["CFO",         na(status.cfo,       fmtHz),   ""],
    ["Cell ID",     status.cell_id  !== undefined ? status.cell_id  : "N/A", ""],
    ["PRB",         status.nof_prb  !== undefined ? status.nof_prb  : "N/A", ""],
    ["SCS",         status.subcarrier_spacing_khz
                      ? status.subcarrier_spacing_khz + " kHz" : "N/A", ""],
  ];

  const t = document.getElementById("sdr-table");
  t.innerHTML = rows.map(([k, v, cls]) =>
    `<tr><td>${k}</td><td class="${cls}">${v}</td></tr>`
  ).join("");
}

function updateChannels(d) {
  const rows = [];

  function addCh(label, ch) {
    if (!ch) {
      rows.push(`<tr class="absent"><td>${label}</td><td>–</td><td>–</td><td>–</td><td>–</td></tr>`);
      return;
    }
    const present = ch.present !== false;
    const cls  = present ? "" : "absent";
    const flag = present
      ? '<span class="flag-ok">✓</span>'
      : '<span class="flag-off">–</span>';
    rows.push(`<tr class="${cls}">
      <td>${label}</td>
      <td>${ch.mcs !== undefined ? ch.mcs : "–"}</td>
      <td>${(ch.ber  || 0).toFixed(5)}</td>
      <td>${(ch.bler || 0).toFixed(5)}</td>
      <td>${flag}</td>
    </tr>`);
  }

  addCh("PDSCH", d.pdsch);
  addCh("MCCH",  d.mcch);

  // MCH: usa mch_info per i TMGI, mch_statuses per MCS/BER/BLER
  if (d.mch_info && d.mch_info.length) {
    d.mch_info.forEach((mch, idx) => {
      const tmgis = (mch.mtchs || [])
        .map(m => m.tmgi).filter(Boolean).join(", ");
      const label = "MCH[" + idx + "]" + (tmgis ? "<br><small>" + tmgis + "</small>" : "");
      addCh(label, d.mch_statuses ? d.mch_statuses[idx] : null);
    });
  } else if (d.mch_statuses) {
    Object.entries(d.mch_statuses).forEach(([idx, st]) => addCh("MCH[" + idx + "]", st));
  }

  document.getElementById("chan-body").innerHTML = rows.join("");
}

// ── Polling ──────────────────────────────────────────────────────────────────

async function fetchData() {
  try {
    const r = await fetch("/data");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const d = await r.json();
    document.getElementById("error").textContent = "";
    updateHeader(d);
    updateSdr(d);
    updateChannels(d);
  } catch (e) {
    document.getElementById("error").textContent = "Errore: " + e.message;
  }
}

// ── Invio frequenza / restart ─────────────────────────────────────────────────

async function sendFreq() {
  const mhz = parseFloat(document.getElementById("freq-input").value);
  if (isNaN(mhz)) return;
  try {
    const r = await fetch("/frequency", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ frequency_mhz: mhz }),
    });
    const notice = document.getElementById("notice");
    if (r.ok) {
      notice.textContent = "Frequenza impostata: " + mhz.toFixed(3) + " MHz";
    } else {
      notice.textContent = "Errore nell'invio";
    }
    setTimeout(() => { notice.textContent = ""; }, 5000);
  } catch (e) {
    document.getElementById("error").textContent = "Errore: " + e.message;
  }
}

// ── Costellazione ────────────────────────────────────────────────────────────

let constChannel = "pdsch";

async function fetchConstellation() {
  const canvas = document.getElementById("const-canvas");
  const ctx    = canvas.getContext("2d");
  const W = canvas.width, H = canvas.height;

  try {
    const r = await fetch("/symbols/" + constChannel);
    if (!r.ok) return;
    const buf   = await r.arrayBuffer();
    const floats = new Float32Array(buf); // [I0, Q0, I1, Q1, ...]
    const npts   = Math.floor(floats.length / 2);
    if (npts === 0) return;

    // Calcola scala automatica sul 95° percentile per escludere outlier
    const mags = [];
    for (let i = 0; i < npts; i++) {
      mags.push(Math.max(Math.abs(floats[2*i]), Math.abs(floats[2*i+1])));
    }
    mags.sort((a, b) => a - b);
    const scale = mags[Math.floor(mags.length * 0.95)] || 1.0;

    // Sfondo + assi
    ctx.fillStyle = "#111";
    ctx.fillRect(0, 0, W, H);
    ctx.strokeStyle = "#333";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(W / 2, 0); ctx.lineTo(W / 2, H); // asse Q
    ctx.moveTo(0, H / 2); ctx.lineTo(W, H / 2); // asse I
    ctx.stroke();

    // Punti IQ con trasparenza per mostrare densità
    ctx.fillStyle = "rgba(102, 187, 106, 0.35)";
    const cx = W / 2, cy = H / 2;
    const r2 = W / 2 / scale;
    for (let i = 0; i < npts; i++) {
      const x = cx + floats[2*i]   * r2;
      const y = cy - floats[2*i+1] * r2; // Q verso l'alto
      ctx.fillRect(x - 1, y - 1, 2, 2);
    }

    // Etichette assi
    ctx.fillStyle = "#555";
    ctx.font = "10px monospace";
    ctx.fillText("I", W - 10, H / 2 - 4);
    ctx.fillText("Q", W / 2 + 4, 10);
    ctx.fillText(npts + " pt", 4, H - 4);

  } catch (_) {}
}

async function sendRestart() {
  try {
    const r = await fetch("/restart", { method: "POST" });
    const notice = document.getElementById("notice");
    notice.textContent = r.ok ? "Restart inviato" : "Errore nel restart";
    setTimeout(() => { notice.textContent = ""; }, 5000);
  } catch (e) {
    document.getElementById("error").textContent = "Errore: " + e.message;
  }
}

// Polling costellazione indipendente (più lento, dati binari)
fetchConstellation();
setInterval(fetchConstellation, 1500);

// Avvia polling ogni secondo
fetchData();
setInterval(fetchData, 1000);
</script>
</body>
</html>
"""

# ── Server multi-thread ───────────────────────────────────────────────────────

class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    """HTTPServer che gestisce ogni richiesta in un thread separato."""
    daemon_threads = True


# ── Cache con thread di background ───────────────────────────────────────────

class DataCache:
    """
    Mantiene in memoria l'ultimo snapshot dei dati del modem.
    Un thread di background aggiorna la cache a intervalli regolari,
    disaccoppiando il ritmo di polling del browser da quello verso il modem.
    """

    # Mappa canale → endpoint binario del modem
    SYMBOL_ENDPOINTS = {
        "pdsch": "pdsch_data",
        "mcch":  "mcch_data",
    }

    def __init__(self, base, data_interval, symbol_interval):
        self._base            = base
        self._data_interval   = data_interval    # secondi tra fetch JSON
        self._symbol_interval = symbol_interval  # secondi tra fetch simboli IQ
        self._lock            = threading.Lock()

        # Valori iniziali vuoti
        self._data    = {}
        self._symbols = {ch: b"" for ch in self.SYMBOL_ENDPOINTS}

        # Avvia i thread di background (daemon: si chiudono con il processo)
        threading.Thread(target=self._loop_data,    daemon=True).start()
        threading.Thread(target=self._loop_symbols, daemon=True).start()

    # ── fetch helpers ─────────────────────────────────────────────────────────

    def _get_json(self, path):
        try:
            return requests.get(f"{self._base}/{path}", timeout=1.5).json()
        except Exception:
            return None

    def _get_raw(self, path):
        try:
            return requests.get(f"{self._base}/{path}", timeout=1.5).content
        except Exception:
            return None

    # ── loop dati JSON ────────────────────────────────────────────────────────

    def _fetch_data(self):
        status   = self._get_json("status")
        sdr      = self._get_json("sdr_params")
        pdsch    = self._get_json("pdsch_status")
        mcch     = self._get_json("mcch_status")
        mch_info = self._get_json("mch_info")

        mch_statuses = {}
        if mch_info:
            for idx in range(len(mch_info)):
                st = self._get_json(f"mch_status/{idx}")
                if st:
                    mch_statuses[idx] = st

        with self._lock:
            self._data = {
                "status":       status,
                "sdr":          sdr,
                "pdsch":        pdsch,
                "mcch":         mcch,
                "mch_info":     mch_info,
                "mch_statuses": mch_statuses,
            }

    def _loop_data(self):
        while True:
            self._fetch_data()
            time.sleep(self._data_interval)

    # ── loop simboli IQ ───────────────────────────────────────────────────────

    def _loop_symbols(self):
        while True:
            for ch, endpoint in self.SYMBOL_ENDPOINTS.items():
                raw = self._get_raw(endpoint)
                if raw is not None:
                    with self._lock:
                        self._symbols[ch] = raw
            time.sleep(self._symbol_interval)

    # ── lettura dalla cache (usata dall'handler HTTP) ─────────────────────────

    def get_data(self):
        with self._lock:
            return dict(self._data)

    def get_symbols(self, channel):
        with self._lock:
            return self._symbols.get(channel)


# ── HTTP handler ──────────────────────────────────────────────────────────────

def make_handler(cache, modem_base):
    """Crea la classe handler con cache e modem_base iniettati via closure."""

    class Handler(BaseHTTPRequestHandler):

        def log_message(self, fmt, *args):
            # Silenzia il log di accesso di default (troppo verboso)
            pass

        def send_json(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", len(body))
            self.end_headers()
            self.wfile.write(body)

        def send_html(self, html):
            body = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", len(body))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            parsed = urlparse(self.path)

            if parsed.path == "/":
                # Serve la pagina HTML principale
                self.send_html(HTML)

            elif parsed.path == "/data":
                # Restituisce l'ultimo snapshot dalla cache (risposta immediata)
                self.send_json(200, cache.get_data())

            elif parsed.path.startswith("/symbols/"):
                # Restituisce gli ultimi simboli IQ dalla cache
                channel = parsed.path.split("/symbols/", 1)[1]
                raw = cache.get_symbols(channel)
                if raw is None:
                    self.send_json(404, {"error": "canale non supportato"})
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", len(raw))
                self.end_headers()
                self.wfile.write(raw)

            else:
                self.send_json(404, {"error": "not found"})

        def do_POST(self):
            parsed = urlparse(self.path)

            if parsed.path == "/restart":
                # Reinvia la frequenza corrente al modem per forzare la risincronizzazione
                data = cache.get_data()
                freq = data.get("sdr", {}) and data["sdr"].get("frequency")
                if not freq:
                    self.send_json(409, {"error": "frequenza non disponibile"})
                    return
                try:
                    r = requests.put(
                        f"{modem_base}/sdr_params",
                        json={"frequency": int(freq)},
                        timeout=2,
                    )
                    if r.status_code == 200:
                        self.send_json(200, {"ok": True})
                    else:
                        self.send_json(502, {"error": "modem returned " + str(r.status_code)})
                except Exception as e:
                    self.send_json(400, {"error": str(e)})
            else:
                self.send_json(404, {"error": "not found"})

        def do_PUT(self):
            parsed = urlparse(self.path)

            if parsed.path == "/frequency":
                # Invia la nuova frequenza direttamente al modem (non passa dalla cache)
                length = int(self.headers.get("Content-Length", 0))
                body   = self.rfile.read(length)
                try:
                    payload = json.loads(body)
                    freq_hz = float(payload["frequency_mhz"]) * 1e6
                    r = requests.put(
                        f"{modem_base}/sdr_params",
                        json={"frequency": int(freq_hz)},
                        timeout=2,
                    )
                    if r.status_code == 200:
                        self.send_json(200, {"ok": True})
                    else:
                        self.send_json(502, {"error": "modem returned " + str(r.status_code)})
                except Exception as e:
                    self.send_json(400, {"error": str(e)})
            else:
                self.send_json(404, {"error": "not found"})

    return Handler

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="MBMS Modem Web Monitor")
    parser.add_argument("modem_base", nargs="?", default=DEFAULT_MODEM,
                        help="URL base del modem API")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="Porta HTTP su cui ascoltare (default: 8080)")
    parser.add_argument("--data-interval", type=float, default=2.0,
                        help="Secondi tra fetch JSON dal modem (default: 2)")
    parser.add_argument("--symbol-interval", type=float, default=3.0,
                        help="Secondi tra fetch simboli IQ dal modem (default: 3)")
    args = parser.parse_args()

    modem_base = args.modem_base.rstrip("/")

    # Avvia la cache con i thread di background
    cache   = DataCache(modem_base, args.data_interval, args.symbol_interval)
    handler = make_handler(cache, modem_base)
    server  = ThreadingHTTPServer(("", args.port), handler)

    print(f"Monitor avviato su http://localhost:{args.port}")
    print(f"Modem API:       {modem_base}")
    print(f"Fetch dati:      ogni {args.data_interval}s")
    print(f"Fetch simboli:   ogni {args.symbol_interval}s")
    print("Premi Ctrl+C per uscire.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer chiuso.")

if __name__ == "__main__":
    main()
