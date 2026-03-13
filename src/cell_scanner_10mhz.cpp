import uhd
import numpy as np
from scipy.signal import find_peaks

def capture_iq_matrix(usrp, center_freq, sample_rate, duration_ms):
    usrp.set_rx_rate(sample_rate)
    usrp.set_rx_freq(uhd.types.TuneRequest(center_freq))
    usrp.set_rx_gain(50)

    # Calcolo campioni necessari
    num_samps = int(sample_rate * (duration_ms / 1000.0))
    print(f"[*] Avvio cattura di {num_samps} campioni ({duration_ms} ms)...")

    st_args = uhd.usrp.StreamArgs("fc32", "sc16")
    streamer = usrp.get_rx_stream(st_args)
    
    # Buffer totale per i 100ms
    buffer = np.zeros(num_samps, dtype=np.complex64)
    metadata = uhd.types.RXMetadata()

    try:
        mode = uhd.types.StreamMode.num_done
    except AttributeError:
        mode = uhd.types.num_done
        
    stream_cmd = uhd.types.StreamCMD(mode)
    stream_cmd.num_samps = num_samps
    stream_cmd.stream_now = True
    
    streamer.issue_stream_cmd(stream_cmd)
    
    # Ricezione robusta a blocchi (per evitare buffer overflow su USB)
    max_samps_per_packet = streamer.get_max_num_samps()
    recv_buffer = np.zeros(max_samps_per_packet, dtype=np.complex64)
    
    idx = 0
    while idx < num_samps:
        n = streamer.recv(recv_buffer, metadata)
        if n == 0:
            break
        take = min(n, num_samps - idx)
        buffer[idx:idx+take] = recv_buffer[:take]
        idx += take

    print("[*] Cattura completata.")
    return buffer

def analyze_matrix(iq_data, sample_rate, center_freq):
    nfft = 4096
    # 1. Tronchiamo il buffer per avere un numero esatto di righe
    num_rows = len(iq_data) // nfft
    iq_data = iq_data[:num_rows * nfft]
    
    # 2. Creazione Matrice (Righe: Tempo, Colonne: Frequenza)
    matrix = iq_data.reshape((num_rows, nfft))
    
    # Applichiamo una finestra (Blackman) per ridurre le sbavature spettrali
    window = np.blackman(nfft)
    
    # 3. Calcolo FFT per ogni riga
    print("[*] Elaborazione matrice STFT...")
    fft_matrix = np.fft.fftshift(np.fft.fft(matrix * window, axis=1), axes=1)
    psd_matrix = 20 * np.log10(np.abs(fft_matrix) + 1e-12)
    
    # 4. Media nel tempo (schiaccia la matrice in un vettore pulitissimo)
    avg_psd = np.mean(psd_matrix, axis=0)
    
    # --- ALGORITMO DI DETECTION ---
    # Risoluzione per ogni "bin" della FFT
    bin_hz = sample_rate / nfft 
    
    # Troviamo i "panettoni" usando SciPy.
    # prominence=10: il segnale deve staccarsi di almeno 10dB dal rumore locale
    # rel_height=0.8: misuriamo la larghezza all'80% dell'altezza del picco (quasi alla base)
    peaks, properties = find_peaks(avg_psd, prominence=10, width=50, rel_height=0.8)
    
    print("\n--- Risultati Analisi Matrice ---")
    canali_trovati = 0
    
    for i, peak_idx in enumerate(peaks):
        # Larghezza in bins * risoluzione Hz = Larghezza in Hz
        width_hz = properties["widths"][i] * bin_hz
        width_mhz = width_hz / 1e6
        
        # Calcolo frequenza centrale esatta
        offset_hz = (peak_idx - (nfft / 2)) * bin_hz
        freq_mhz = (center_freq + offset_hz) / 1e6
        
        potenza_db = avg_psd[peak_idx]
        
        # Filtriamo solo i canali di nostro interesse (circa 5 o 8 MHz)
        if 4.0 <= width_mhz <= 6.0:
            tipo = "Probabile FeMBMS/LTE (5 MHz)"
        elif 7.0 <= width_mhz <= 9.0:
            tipo = "Probabile DVB-T2 (8 MHz)"
        else:
            tipo = "Altro segnale"

        # Mostriamo solo se assomiglia a un canale noto
        if tipo != "Altro segnale":
            canali_trovati += 1
            print(f"[{canali_trovati}] {tipo}")
            print(f"    Centro:    {freq_mhz:.3f} MHz")
            print(f"    Larghezza: {width_mhz:.2f} MHz")
            print(f"    Potenza:   {potenza_db:.1f} dB\n")

    if canali_trovati == 0:
        print("Nessun canale FeMBMS o TV rilevato in questo blocco da 40 MHz.")

# --- MAIN ---
if __name__ == "__main__":
    try:
        print("\n--- Inizializzazione SDR ---")
        usrp_obj = uhd.usrp.MultiUSRP("type=b200")
        
        # Scansioniamo a 640 MHz (copre da 620 a 660 MHz)
        freq_test = 640e6
        sr = 40e6
        
        iq_buffer = capture_iq_matrix(usrp_obj, freq_test, sr, duration_ms=100)
        analyze_matrix(iq_buffer, sr, freq_test)

    except Exception as e:
        print(f"Errore: {e}")