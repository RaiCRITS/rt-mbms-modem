#!/usr/bin/env python3
"""
MBMS Modem CLI Monitor
Dipendenze: pip install rich requests
Uso: python3 modem_monitor.py [http://host:porta/modem-api]
"""

import sys
import time
import select
import tty
import termios
import requests
from rich.console import Console, Group
from rich.table import Table
from rich.live import Live
from rich.text import Text
from rich.columns import Columns
from rich.panel import Panel
from rich.rule import Rule
from rich import box

# Indirizzo REST del modem se non specificato da riga di comando
DEFAULT_BASE = "http://127.0.0.1:3010/modem-api"

# Colori associati ai possibili stati del modem
STATE_COLORS = {
    "searching":    "yellow",
    "syncing":      "cyan",
    "synchronized": "green",
}


# ── API helpers ───────────────────────────────────────────────────────────────

def api_get(base, path):
    """GET su un endpoint REST del modem; restituisce il JSON o None in caso di errore."""
    try:
        return requests.get(f"{base}/{path}", timeout=1.5).json()
    except Exception:
        return None

def api_put_freq(base, freq_hz):
    """Invia una nuova frequenza centrale al modem via PUT /sdr_params."""
    try:
        r = requests.put(f"{base}/sdr_params",
                         json={"frequency": int(freq_hz)}, timeout=2)
        return r.status_code == 200
    except Exception:
        return False

def read_key_nonblocking():
    """Legge un tasto da stdin senza bloccare; restituisce None se non c'è input."""
    if select.select([sys.stdin], [], [], 0)[0]:
        return sys.stdin.read(1)
    return None


# ── Formatters ────────────────────────────────────────────────────────────────

def fmt_freq(hz):
    """Frequenza in MHz con 4 decimali (es. 943.2000 MHz)."""
    return f"{hz/1e6:.4f} MHz" if hz else "N/A"

def fmt_mhz(hz):
    """Valore in MHz con 3 decimali, usato per sample rate e filter BW."""
    return f"{hz/1e6:.3f} MHz" if hz else "N/A"

def fmt_db(v):
    """Valore in dB con 2 decimali."""
    return f"{v:.2f} dB" if v is not None else "N/A"

def fmt_hz(v):
    """Offset in Hz con segno (es. +12.3 Hz), usato per il CFO."""
    return f"{v:+.1f} Hz" if v is not None else "N/A"

def fmt_pct(v):
    """Valore 0..1 come percentuale (es. 0.73 → 73.0%)."""
    return f"{v*100:.1f}%" if v is not None else "N/A"


# ── Display ───────────────────────────────────────────────────────────────────

def make_sdr_panel(sdr, status):
    """
    Pannello sinistro: parametri SDR (frequenza, gain, sample rate, buffer…)
    e metriche RF (CINR, CFO, cell ID, PRB, SCS) provenienti dallo status.
    """
    t = Table(box=box.SIMPLE, show_header=False, padding=(0, 1))
    t.add_column("k", style="dim", width=12, no_wrap=True)
    t.add_column("v")

    def row(k, v, style=""):
        t.add_row(k, Text(str(v), style=style) if style else str(v))

    # Parametri SDR
    freq = sdr.get("frequency") if sdr else None
    row("Frequenza",   fmt_freq(freq),                               "bold white")
    row("Gain",        fmt_db(sdr.get("gain"))         if sdr else "N/A")
    row("Sample rate", fmt_mhz(sdr.get("sample_rate")) if sdr else "N/A")
    row("Filter BW",   fmt_mhz(sdr.get("filter_bw"))  if sdr else "N/A")
    row("Buffer",      fmt_pct(sdr.get("buffer_level")) if sdr else "N/A")
    row("Antenna",     sdr.get("antenna", "N/A")        if sdr else "N/A")

    # Metriche RF dallo status
    cinr = status.get("cinr_db") if status else None
    cfo  = status.get("cfo")     if status else None
    # CINR verde se > 10 dB (buona ricezione), giallo altrimenti
    row("CINR", fmt_db(cinr), "bold green" if cinr and cinr > 10 else "bold yellow")
    row("CFO",  fmt_hz(cfo))
    if status:
        row("Cell ID", status.get("cell_id", "N/A"))
        row("PRB",     status.get("nof_prb",  "N/A"))
        scs = status.get("subcarrier_spacing_khz")
        row("SCS", f"{scs} kHz" if scs else "N/A")

    return Panel(t, title="[bold cyan]SDR / RF[/]", border_style="cyan")


def make_channels_panel(pdsch, mcch, mch_info, mch_statuses):
    """
    Pannello destro: tabella dei canali (PDSCH, MCCH, MCH[n]) con MCS, BER e BLER.
    Per i canali MCH mostra anche i TMGI degli MTCH associati se disponibili.
    """
    t = Table(box=box.SIMPLE, show_header=True, padding=(0, 1))
    t.add_column("Channel", style="bold", width=14, no_wrap=True)
    t.add_column("MCS",  justify="right", width=5)
    t.add_column("BER",  justify="right", width=10)
    t.add_column("BLER", justify="right", width=10)
    t.add_column("",     width=2)  # flag presente/assente

    def ch_row(label, data):
        """Aggiunge una riga canale; se data è None il canale non è ricevuto."""
        if data:
            present = data.get("present", True)
            style   = "" if present else "dim"
            flag    = Text("✓", style="green") if present else Text("–", style="dim")
            t.add_row(
                Text(label, style=style),
                str(data.get("mcs", "–")),
                f"{data.get('ber',  0):.5f}",
                f"{data.get('bler', 0):.5f}",
                flag,
            )
        else:
            t.add_row(Text(label, style="dim"), "–", "–", "–", "–")

    ch_row("PDSCH", pdsch)
    ch_row("MCCH",  mcch)

    # MCH: usa mch_info per ricavare i TMGI, poi lo stato da mch_statuses
    if mch_info:
        for idx, mch in enumerate(mch_info):
            tmgis = ", ".join(m.get("tmgi", "") for m in mch.get("mtchs", []) if m.get("tmgi"))
            label = f"MCH[{idx}]" + (f"\n{tmgis}" if tmgis else "")
            ch_row(label, mch_statuses.get(idx))
    elif mch_statuses:
        # fallback se mch_info non è disponibile
        for idx, st in mch_statuses.items():
            ch_row(f"MCH[{idx}]", st)

    return Panel(t, title="[bold magenta]Canali[/]", border_style="magenta")


def make_display(status, sdr, pdsch, mcch, mch_info, mch_statuses, notice):
    """
    Compone l'intera schermata:
      - Rule in cima con stato e frequenza corrente
      - Due pannelli affiancati (SDR/RF | Canali)
      - Footer con i tasti disponibili e gli eventuali notice temporanei
    """
    state = status.get("state", "?") if status else "N/A"
    color = STATE_COLORS.get(state, "red")
    freq  = sdr.get("frequency") if sdr else None

    footer_parts = "[dim][bold]f[/bold]=freq  [bold]r[/bold]=restart  [bold]q[/bold]=esci[/dim]"
    if notice:
        footer_parts += f"   [bold yellow]{notice}[/]"

    state_markup = f"[bold {color}]● {state.upper()}[/]"
    freq_markup  = f"[bold white]{fmt_freq(freq)}[/]"

    return Group(
        Rule(f"[bold]MBMS Monitor[/]  {state_markup}  {freq_markup}"),
        Columns([
            make_sdr_panel(sdr, status),
            make_channels_panel(pdsch, mcch, mch_info, mch_statuses),
        ], expand=True),
        Text.from_markup(footer_parts, justify="center"),
    )


# ── Input ─────────────────────────────────────────────────────────────────────

def ask_frequency(console, current_hz):
    """Chiede interattivamente una nuova frequenza in MHz; restituisce Hz o None."""
    console.print()
    if current_hz:
        console.print(f"[dim]Frequenza attuale: {fmt_freq(current_hz)}[/dim]")
    console.print("[cyan]Nuova frequenza (MHz), es. 943.2 — Invio per annullare:[/cyan] ", end="")
    try:
        raw = input()
        if raw.strip():
            return float(raw.strip()) * 1e6
    except (ValueError, EOFError):
        pass
    return None


# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    base = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else DEFAULT_BASE
    console = Console()
    notice = ""        # messaggio temporaneo mostrato nel footer
    notice_until = 0.0 # timestamp fino a cui mostrare il notice

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)  # salva impostazioni terminale originali

    try:
        tty.setraw(fd)   # modalità raw: i tasti sono letti senza buffering né echo
        console.clear()  # pulizia iniziale per evitare artefatti di testo preesistente

        # auto_refresh=False: Rich ridisegna solo quando chiamiamo live.update(..., refresh=True),
        # evitando ridisegni asincroni che causerebbero lampeggio
        with Live(console=console, auto_refresh=True) as live:
            while True:
                # Raccoglie tutti i dati dal modem (chiamate sequenziali)
                status   = api_get(base, "status")
                sdr      = api_get(base, "sdr_params")
                pdsch    = api_get(base, "pdsch_status")
                mcch     = api_get(base, "mcch_status")
                mch_info = api_get(base, "mch_info")

                # Recupera lo stato per ogni MCH trovato in mch_info
                mch_statuses = {}
                if mch_info:
                    for idx in range(len(mch_info)):
                        st = api_get(base, f"mch_status/{idx}")
                        if st:
                            mch_statuses[idx] = st

                # Scade il notice dopo il timeout impostato
                if notice and time.time() > notice_until:
                    notice = ""

                live.update(make_display(status, sdr, pdsch, mcch,
                                        mch_info, mch_statuses, notice),
                            refresh=True)

                key = read_key_nonblocking()

                if key == "q":
                    break

                elif key == "f":
                    # Sospende il Live per leggere input da tastiera in modo normale
                    live.stop()
                    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                    console.clear()

                    current = sdr.get("frequency") if sdr else None
                    new_hz  = ask_frequency(console, current)

                    if new_hz is not None:
                        console.print(f"[cyan]Invio {fmt_freq(new_hz)}…[/cyan]")
                        ok = api_put_freq(base, new_hz)
                        notice = f"Freq impostata: {fmt_freq(new_hz)}" if ok else "Errore nell'invio freq"
                    else:
                        notice = "Operazione annullata"

                    notice_until = time.time() + 5
                    tty.setraw(fd)
                    live.start()

                elif key == "r":
                    # Reinvia la frequenza corrente per forzare un restart della sincronizzazione
                    if sdr and sdr.get("frequency"):
                        ok = api_put_freq(base, sdr["frequency"])
                        notice = "Restart inviato" if ok else "Errore nel restart"
                    else:
                        notice = "Frequenza non disponibile"
                    notice_until = time.time() + 5

                time.sleep(0.5)  # pausa tra un ciclo e il successivo (~2 refresh/s)

    finally:
        # Ripristina sempre le impostazioni originali del terminale all'uscita
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        console.print("\n[dim]Monitor chiuso.[/dim]")


if __name__ == "__main__":
    main()
