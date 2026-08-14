# Correzioni da applicare in `lib/srsran` — da fare

Note prese durante l'analisi del 14/08/2026. Nessuna di queste è ancora applicata.

`lib/srsran` **non è srsRAN upstream**: contiene già le modifiche FeMBMS del progetto
(enum `srsran_scs_t`, funzioni reference signal multi-SCS, commenti `// [kku]`).
Correggere questi punti significa sistemare una patch già locale, non introdurre
una nuova divergenza dall'upstream.

---

## 1. Stima del rumore MBSFN rotta → CINR MBSFN sempre NaN

**Sintomo osservato:** `CINR: CAS 17.43 dB, MBSFN -nan dB` nei log.
Dopo l'aggiunta della guardia `isfinite` in `RestHandler::ema_update()` il valore
si presenta come `0.00 dB`, che ora significa "nessun campione valido".

### Causa

`refsignal_dl.h:91` dichiara la funzione **senza parametri**, mentre
`refsignal_dl.c:350` la definisce **con** il parametro SCS:

```c
// refsignal_dl.h:91
SRSRAN_API uint32_t srsran_refsignal_mbsfn_nof_symbols();

// refsignal_dl.c:350
uint32_t srsran_refsignal_mbsfn_nof_symbols(srsran_scs_t scs)
```

In C `()` significa "parametri non specificati" (forma K&R), non "nessun parametro"
(che sarebbe `(void)`). Il compilatore quindi non segnala nulla e la chiamata senza
argomenti in `chest_dl.c:342` passa in silenzio.

Catena completa dell'errore:

1. `chest_dl.c:342` chiama `srsran_refsignal_mbsfn_nof_symbols()` senza argomenti
2. `scs` legge un registro non inizializzato → lo `switch` cade su `default: return 0`
3. `if (nsymbols == 0) { ERROR(...); return SRSRAN_ERROR; }` → ritorna **-1**
4. il -1 diventa `noise_estimate`, quindi `snr = rsrp / (-1)` è negativo
5. `srsran_convert_power_to_dB()` è `10 * log10f(v)` → log di negativo = **NaN**

Secondo difetto, `chest_dl.c:351`: l'SCS è **cablato a 15 kHz**
(`srsran_refsignal_mbsfn_fidx(1, SRSRAN_SCS_15KHZ)`), ignorando quello reale.

Indizio che la patch FeMBMS è rimasta incompleta: in `chest_dl.c:342` l'autore usa
`sf->subcarrier_spacing` per il termine `+ (… == SRSRAN_SCS_15KHZ ? 1 : 0)`, quindi
aveva l'SCS a disposizione, ma non lo passa alla funzione che ne ha bisogno.

`refsignal_dl.c` è invece corretto e completo per tutti gli SCS:

| SCS      | simboli pilota | RS per simbolo |
|----------|----------------|----------------|
| 15 kHz   | 3              | 6              |
| 7,5 kHz  | 3              | 6              |
| 2,5 kHz  | 2              | 18             |
| 1,25 kHz | 1              | 24             |
| 0,37 kHz | 1              | 40             |

### Correzioni

1. **`lib/srsran/lib/include/srsran/phy/ch_estimation/refsignal_dl.h:91`**
   → dichiarare `srsran_refsignal_mbsfn_nof_symbols(srsran_scs_t scs)`.
   Da sola questa riga trasforma l'UB silenzioso in errore di compilazione su ogni
   chiamata sbagliata: è il modo giusto per trovarle tutte. **Farla per prima.**

2. **`lib/srsran/lib/src/phy/ch_estimation/chest_dl.c:342`**
   → passare `sf->subcarrier_spacing`.

3. **`lib/srsran/lib/src/phy/ch_estimation/chest_dl.c:351`**
   → passare `sf->subcarrier_spacing` invece di `SRSRAN_SCS_15KHZ` cablato.

4. Poi, lato modem: `src/MbsfnFrameProcessor.cpp:52` da `SRSRAN_NOISE_ALG_EMPTY` a
   `SRSRAN_NOISE_ALG_REFS`, e rimuovere il warning ora spurio in `chest_dl.c:706`
   (`"REFS noise estimation algorithm not supported in MBSFN subframes"`), che
   altrimenti sarebbe emesso a livello ERROR su ogni subframe MBSFN.

`NOISE_ALG_EMPTY` salta l'MBSFN per costruzione (`ch_mode != SRSRAN_SF_MBSFN` in
`chest_dl.c:783`), quindi finché resta configurato il CINR MBSFN è impossibile.

### Da verificare sul campo (non validabile a tavolino)

- Il `+1` per i 15 kHz porta `nsymbols` a 4, coerente con `input2d[4]`.
- Per gli SCS ridotti che danno 1 solo simbolo si attiva il ramo speciale
  `if (nsymbols < 3)` in `chest_dl.c:355`, che stima il rumore in frequenza anziché
  in tempo: plausibile, ma da controllare con i numeri reali.

### Non toccare la configurazione del CAS

`NOISE_ALG_EMPTY` sul CAS **funziona** (verificato: 17,4 dB stabili). L'`inf` visto
inizialmente era un transitorio d'avvio — la stima del rumore è calcolata solo sui
subframe 0 e 5, prima di allora `noise_estimate` è 0 → `snr = rsrp/0 = inf` — che
la media esponenziale congelava. Risolto dalla guardia `isfinite`.

---

## 2. `max_nof_iterations` non applicato al PMCH

**Sintomo:** `n_iter 10.0` su MCCH e MCH, nonostante
`src/MbsfnFrameProcessor.cpp:65` imposti `_pmch_cfg.pdsch_cfg.max_nof_iterations = 8`.

**Causa:** `srsran_sch_set_max_noi()` è chiamata solo da `pdsch.c:823` e
`pusch.c:450`, **mai da `pmch.c`**. Per il PMCH resta quindi il default
`SRSRAN_PDSCH_MAX_TDEC_ITERS = 10` (`sch.c:36`).

**Correzione:** aggiungere la chiamata a `srsran_sch_set_max_noi()` nel percorso di
decodifica di `pmch.c`, come fa `pdsch.c`.

**Priorità bassa:** incide solo sul tetto di iterazioni (10 invece di 8), quindi su
un po' di CPU sprecata quando la decodifica non converge, non sulla capacità di
decodificare.

---

## Nota: perché il BER non si ripristina allo stesso modo

Il BER (`ChannelInfo::ber`, oggi fisso a 0) richiederebbe di **reimplementare** in
srsRAN la ri-codifica del transport block seguita da rate matching — non la
correzione di un bug esistente, ma una funzionalità che nello srsRAN attuale non
c'è proprio (`srsran_softbuffer_rx_t` non ha alcun campo `ber`). Era una patch al
fork srsLTE del progetto, persa nel rebase a srsRAN. Costo CPU nell'hot path da
misurare prima di impegnarsi. Vedi i commit storici `29a5233` e `6ff1174`.
