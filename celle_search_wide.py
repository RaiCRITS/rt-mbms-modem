import uhd
import numpy as np
from scipy import signal

def zadoff_chu(u, N_zc=63):
    n = np.arange(N_zc)
    return np.exp(-1j * np.pi * u * n * (n + 1) / N_zc)

def get_pss_templates():
    # Genera i 3 PSS standard (62 campioni centrali)
    roots = [25, 29, 34]
    templates = []
    for u in roots:
        # Creiamo il template e aggiungiamo gli zeri per portarlo a 128 campioni (FFT size)
        zc = zadoff_chu(u)
        templates.append(np.conj(zc[::-1]))
    return templates

def high_sensitivity_scan(samples, fs):
    templates = get_pss_templates()
    target_fs = 1.92e6
    decimation = int(fs / target_fs)
    # Filtro passa-basso prima della decimazione per non tirare dentro rumore
    resampled = signal.decimate(samples, decimation, ftype='fir')
    
    best_metric = 0
    found_id = -1
    
    # --- STEP DI SENSIBILITÀ: Ricerca Frequency Offset ---
    # Proviamo a spostare il segnale di +/- 5kHz a passi di 1kHz
    for freq_shift in range(-5000, 6000, 1000):
        t = np.arange(len(resampled)) / target_fs
        shifted_data = resampled * np.exp(-1j * 2 * np.pi * freq_shift * t)
        
        for pss_id, temp in enumerate(templates):
            corr = signal.correlate(shifted_data, temp, mode='valid')
            mag_corr = np.abs(corr)
            
            # Calcolo del Peak-to-Average Power Ratio (PAPR) della correlazione
            # È una metrica molto più sensibile della semplice magnitudo
            papr = np.max(mag_corr)**2 / np.mean(mag_corr**2)
            
            if papr > best_metric:
                best_metric = papr
                found_id = pss_id
                
    return found_id, best_metric

# --- MAIN ---
usrp = uhd.usrp.MultiUSRP("type=b200")
fs = 30.72e6 
usrp.set_rx_rate(fs)
usrp.set_rx_gain(70) # Alza il guadagno! Per LTE serve almeno 60-70dB

# Lista frequenze canali EARFCN comuni in Italia (es. B20 o B3)
# Esempio: 806 MHz (Vodafone), 796 MHz (TIM)
freqs_to_test = [610e6, 620e6, 630e6, 1815e6, 1840e6]

for f in freqs_to_test:
    usrp.set_rx_freq(uhd.types.TuneRequest(f))
    print(f"Scansione profonda a {f/1e6} MHz...")
    
    # Cattura 40ms per avere 8 tentativi di PSS (uno ogni 5ms)
    samples = usrp.recv_num_samps(int(fs * 0.04), f, fs, [0], 50)[0]
    
    pss_id, score = high_sensitivity_scan(samples, fs)
    
    # Con il PAPR, una soglia di 15-20 indica solitamente una cella reale
    if score > 15:
        print(f">>> [TROVATA] PSS_ID: {pss_id} | Confidence: {score:.2f}")
    else:
        print(f"Nessun segnale chiaro (Best: {score:.2f})")