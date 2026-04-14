/**
 * mbms_thread_benchmark.cpp  v2
 *
 * Misura due cose distinte, che nel modem hanno ruoli diversi:
 *
 * TEST 1 — LATENZA SINGOLO SUBFRAME
 *   Risponde a: "Un singolo frame processor riesce a finire entro 10ms?"
 *   Metodologia: esegue process_subframe() in serie, misura ogni singola
 *   chiamata. Se il P99 supera 10ms il singolo thread non basta, ma piu'
 *   thread in parallelo possono compensare (i subframe MBSFN sono indipendenti).
 *   Solo se anche il Test 2 con tutti i thread disponibili fallisce, l'hardware
 *   non e' adeguato per la configurazione PRB scelta.
 *
 * TEST 2 — PIPELINE DEPTH (quanti MBSFN processor tenere pronti)
 *   Risponde a: "Quanti frame posso elaborare in parallelo senza che
 *   il pool diventi il collo di bottiglia?"
 *   Metodologia: simula il pattern reale del main loop — un nuovo job
 *   viene lanciato ogni ~10ms (=budget LTE), e si misura quanti job
 *   contemporanei il pool riesce a smaltire senza accumulare backlog.
 *   Questo numero corrisponde a modem.phy.threads.
 *
 * Compilazione:
 *   g++ -O2 -std=c++17 -pthread mbms_thread_benchmark.cpp -o mbms_benchmark
 *
 * Uso:
 *   ./mbms_benchmark [--prb N] [--max-threads N] [--samples N] [--cas] [--turbo N]
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cmath>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define COL_RESET  "\033[0m"
#define COL_RED    "\033[31m"
#define COL_GREEN  "\033[32m"
#define COL_YELLOW "\033[33m"
#define COL_CYAN   "\033[36m"
#define COL_BOLD   "\033[1m"

static constexpr double LTE_BUDGET_MS  = 10.0;
static constexpr double SAFE_BUDGET_MS =  8.5;

// PRB → campioni per subframe (tabella srsran)
static int prb_to_samples(int prb) {
    if (prb <= 6)  return 1920;
    if (prb <= 15) return 3840;
    if (prb <= 25) return 7680;
    if (prb <= 50) return 15360;
    if (prb <= 75) return 23040;
    return 30720;
}

using cf_t = std::complex<float>;

struct WorkloadParams {
    int  nof_prb     = 25;
    int  nof_symbols = 14;
    int  turbo_iters = 8;
    bool is_mbsfn    = true;
};

// ─── Workload (identico a v1) ─────────────────────────────────────────────────

static void sim_fft(std::vector<cf_t>& buf) {
    int n = (int)buf.size();
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * 3.14159265f / (float)len;
        cf_t wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            cf_t w(1.0f, 0.0f);
            for (int j = 0; j < len/2; j++) {
                cf_t u = buf[i+j], v = buf[i+j+len/2] * w;
                buf[i+j] = u+v; buf[i+j+len/2] = u-v; w *= wlen;
            }
        }
    }
}
static void sim_chest(std::vector<cf_t>& ce, const std::vector<cf_t>& pilots, int nre) {
    int np = (int)pilots.size();
    if (np < 2) return;
    int step = std::max(1, nre / np);
    for (int i = 0; i < np-1; i++) {
        cf_t delta = (pilots[i+1]-pilots[i]) * (1.0f/(float)step);
        for (int j = 0; j < step; j++) { int idx=i*step+j; if(idx<nre) ce[idx]=pilots[i]+delta*(float)j; }
    }
}
static void sim_equalizer(std::vector<cf_t>& rx, const std::vector<cf_t>& ce, float nv) {
    int nce=(int)ce.size();
    for (int i=0;i<(int)rx.size();i++) { cf_t h=ce[i%nce]; rx[i]=(rx[i]*std::conj(h))/(std::norm(h)+nv); }
}
static volatile float _sink = 0;
static void sim_turbo(std::vector<float>& llr, int iters) {
    int n=(int)llr.size();
    for (int it=0;it<iters;it++) {
        for (int i=1;i<n;i++) llr[i]=std::tanh(0.5f*(llr[i]+llr[i-1]*0.3f));
        for (int i=n-2;i>=0;i--) llr[i]=std::tanh(0.5f*(llr[i]+llr[i+1]*0.3f));
    }
    float s=0; for(auto v:llr) s+=std::abs(v); _sink=s;
}

static double process_subframe(const WorkloadParams& p) {
    auto t0 = std::chrono::steady_clock::now();
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1.0f,1.0f);
    int nsamples=prb_to_samples(p.nof_prb), nre=p.nof_prb*12*p.nof_symbols, npilots=p.nof_prb*(p.is_mbsfn?4:2);
    std::vector<cf_t> rx_samples(nsamples);
    for(auto& s:rx_samples) s=cf_t(dist(rng),dist(rng));
    int fft_sz=nsamples/p.nof_symbols, fft_p2=1;
    while(fft_p2*2<=fft_sz) fft_p2<<=1;
    std::vector<cf_t> fft_buf(fft_p2);
    for(int sym=0;sym<p.nof_symbols;sym++){
        int off=sym*(nsamples/p.nof_symbols);
        for(int i=0;i<fft_p2&&(off+i)<nsamples;i++) fft_buf[i]=rx_samples[off+i];
        sim_fft(fft_buf);
    }
    std::vector<cf_t> pilots(npilots); for(auto& pp:pilots) pp=cf_t(dist(rng),dist(rng));
    std::vector<cf_t> ce(nre,cf_t(1,0)); sim_chest(ce,pilots,nre);
    std::vector<cf_t> rx_re(nre); for(auto& r:rx_re) r=cf_t(dist(rng),dist(rng));
    sim_equalizer(rx_re,ce,0.1f);
    int tbs=p.nof_prb*100;
    std::vector<float> llr(tbs);
    for(int i=0;i<tbs;i++) llr[i]=std::real(rx_re[i%nre])*2.0f;
    sim_turbo(llr,p.turbo_iters);
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
}

// ─── Thread pool ──────────────────────────────────────────────────────────────
class Pool {
    using Task = std::function<void()>;
public:
    explicit Pool(int n) : _stop(false), _active(0) {
        for(int i=0;i<n;i++) _workers.emplace_back([this]{loop();});
    }
    ~Pool() {
        {std::unique_lock<std::mutex> lk(_mx);_stop=true;}
        _cv.notify_all();
        for(auto& t:_workers) if(t.joinable()) t.join();
    }
    std::future<double> submit(std::function<double()> fn) {
        auto sp=std::make_shared<std::packaged_task<double()>>(std::move(fn));
        auto fut=sp->get_future();
        {std::unique_lock<std::mutex> lk(_mx); _q.push(Task([sp]{(*sp)();}));}
        _cv.notify_one();
        return fut;
    }
    int queue_depth() { std::unique_lock<std::mutex> lk(_mx); return (int)_q.size(); }
    int active() { return _active.load(); }
private:
    void loop() {
        for(;;){
            Task t;
            {std::unique_lock<std::mutex> lk(_mx);_cv.wait(lk,[this]{return _stop||!_q.empty();});
             if(_stop&&_q.empty()) return; t=std::move(_q.front());_q.pop();}
            ++_active; t(); --_active;
        }
    }
    std::vector<std::thread> _workers;
    std::queue<Task> _q;
    std::mutex _mx; std::condition_variable _cv;
    std::atomic<bool> _stop; std::atomic<int> _active;
};

// ─── Statistiche ──────────────────────────────────────────────────────────────
struct Stats {
    double mean, p50, p95, p99, max;
    static Stats compute(std::vector<double> v) {
        std::sort(v.begin(),v.end());
        double sum=std::accumulate(v.begin(),v.end(),0.0);
        return { sum/v.size(),
                 v[v.size()*50/100], v[v.size()*95/100],
                 v[v.size()*99/100], v.back() };
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// TEST 1: latenza singolo subframe (serie, senza parallelismo)
//
// Simula: il main loop chiama process() su UN processore alla volta.
// Se il P99 supera 10ms, il singolo thread non basta — ma i subframe MBSFN
// sono indipendenti, quindi piu' thread in parallelo possono compensare.
// Il Test 2 determina quanti thread sono effettivamente necessari.
// ═════════════════════════════════════════════════════════════════════════════
static Stats test1_single_latency(const WorkloadParams& p, int nsamples) {
    std::vector<double> times;
    times.reserve(nsamples);
    for(int i=0;i<nsamples;i++) times.push_back(process_subframe(p));
    return Stats::compute(times);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 2: pipeline depth — quanti job in parallelo il pool smaltisce senza
//         accumulare backlog.
//
// Simula il pattern reale: il main loop lancia un job ogni ~subframe_interval_ms
// (nel modem reale: 10ms per MBSFN, 40ms per CAS in dedicated mode).
// Con N thread nel pool, finché ogni job finisce prima che arrivi il prossimo,
// non si accumula backlog. Quando il backlog cresce significa che i thread
// non bastano e il modem inizia a perdere frame.
//
// Metrica chiave: "backlog_at_end" — se > 0 la configurazione non regge.
// ═════════════════════════════════════════════════════════════════════════════
struct PipelineResult {
    int    threads;
    double mean_ms;        // tempo medio elaborazione singolo job
    double p99_ms;
    int    backlog_at_end; // job in coda alla fine: >0 = non regge
    int    max_queue_depth;
    bool   ok;             // true se backlog_at_end == 0
};

static PipelineResult test2_pipeline(int nthreads, const WorkloadParams& p,
                                     int njobs, double interval_ms) {
    Pool pool(nthreads);
    std::vector<double> job_times;
    job_times.reserve(njobs);
    std::mutex tmx;
    std::atomic<int> max_depth{0};

    for(int i=0;i<njobs;i++) {
        // Simula il main loop: lancia job ogni interval_ms
        pool.submit([&p, &job_times, &tmx]() -> double {
            double t = process_subframe(p);
            {std::lock_guard<std::mutex> lk(tmx); job_times.push_back(t);}
            return t;
        });

        // Aggiorna max queue depth
        int qd = pool.queue_depth() + pool.active();
        int cur = max_depth.load();
        while(qd > cur && !max_depth.compare_exchange_weak(cur, qd));

        // Aspetta interval_ms prima di lanciare il prossimo job
        // (simula il timing reale del main loop LTE)
        std::this_thread::sleep_for(
            std::chrono::duration<double, std::milli>(interval_ms));
    }

    // Aspetta che tutti i job finiscano
    // (nel modem reale, se ci sono job in coda qui si perdono frame)
    auto wait_start = std::chrono::steady_clock::now();
    int final_backlog = 0;
    while(pool.active() > 0 || pool.queue_depth() > 0) {
        final_backlog = pool.queue_depth() + pool.active();
        double waited = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-wait_start).count();
        if(waited > LTE_BUDGET_MS * njobs) break; // safety
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    final_backlog = pool.queue_depth() + pool.active();

    Stats s = Stats::compute(job_times);
    return { nthreads, s.mean, s.p99, final_backlog,
             max_depth.load(), final_backlog == 0 };
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    WorkloadParams p;
    int max_threads = std::min(8, (int)std::thread::hardware_concurrency());
    int nsamples    = 300;
    double interval_ms = LTE_BUDGET_MS;  // 10ms = 1 subframe LTE

    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        if      (a=="--prb"         && i+1<argc) p.nof_prb     = std::stoi(argv[++i]);
        else if (a=="--max-threads" && i+1<argc) max_threads   = std::stoi(argv[++i]);
        else if (a=="--samples"     && i+1<argc) nsamples      = std::stoi(argv[++i]);
        else if (a=="--turbo"       && i+1<argc) p.turbo_iters = std::stoi(argv[++i]);
        else if (a=="--interval"    && i+1<argc) interval_ms   = std::stod(argv[++i]);
        else if (a=="--cas")                      p.is_mbsfn   = false;
        else if (a=="--help"){
            std::cout <<
                "Uso: mbms_benchmark [opzioni]\n"
                "  --prb N          PRB (6/15/25/50/75/100, default: 25 = 5MHz)\n"
                "  --max-threads N  Max thread da testare nel Test 2 (default: min(8,nCPU))\n"
                "  --samples N      Subframe per misura (default: 300)\n"
                "  --turbo N        Iterazioni turbo decoder (default: 8)\n"
                "  --interval N     ms tra un subframe e l'altro nel Test 2 (default: 10)\n"
                "  --cas            Simula workload CAS invece di MBSFN\n";
            return 0;
        }
    }

    unsigned ncpu = std::thread::hardware_concurrency();

    std::cout << COL_BOLD
        << "\n========================================\n"
        << "  MBMS Modem - Thread Benchmark v2\n"
        << "========================================\n" << COL_RESET
        << "CPU logici: " << ncpu
        << " | PRB: " << p.nof_prb
        << " (" << p.nof_prb*200 << " kHz)"
        << " | Tipo: " << (p.is_mbsfn?"MBSFN":"CAS")
        << " | Turbo iter: " << p.turbo_iters << "\n\n";

    // Warm-up
    std::cout << "Warm-up..." << std::flush;
    for(int i=0;i<10;i++) process_subframe(p);
    std::cout << " ok\n\n";

    // ── TEST 1 ────────────────────────────────────────────────────────────────
    std::cout << COL_BOLD
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║ TEST 1: Latenza singolo subframe (elaborazione in serie) ║\n"
        << "║  Domanda: un frame processor sta nel budget da solo?     ║\n"
        << "╚══════════════════════════════════════════════════════════╝\n"
        << COL_RESET;
    std::cout << "Misuro " << nsamples << " subframe consecutivi...\n\n";

    Stats s1 = test1_single_latency(p, nsamples);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Media:   " << s1.mean << " ms\n";
    std::cout << "  P50:     " << s1.p50  << " ms\n";
    std::cout << "  P95:     " << s1.p95  << " ms\n";
    std::cout << "  P99:     " << s1.p99  << " ms\n";
    std::cout << "  Max:     " << s1.max  << " ms\n";
    std::cout << "  Budget:  " << LTE_BUDGET_MS << " ms\n\n";

    bool t1_ok = s1.p99 < SAFE_BUDGET_MS;
    if(t1_ok) {
        std::cout << COL_GREEN << COL_BOLD
            << "  ✓ OK — il singolo frame processor sta nel budget (P99 "
            << s1.p99 << " ms < " << SAFE_BUDGET_MS << " ms)\n"
            << "    L'hardware e' adeguato. Procedi al Test 2.\n"
            << COL_RESET;
    } else if(s1.p99 < LTE_BUDGET_MS) {
        std::cout << COL_YELLOW << COL_BOLD
            << "  ~ TIGHT — il singolo frame processor e' ai limiti\n"
            << "    (P99 " << s1.p99 << " ms). Sotto carico OS potresti\n"
            << "    perdere frame. Valuta kernel PREEMPT_RT.\n"
            << COL_RESET;
    } else {
        std::cout << COL_RED << COL_BOLD
            << "  ! ATTENZIONE — il singolo frame processor supera il budget!\n"
            << "    (P99 " << s1.p99 << " ms > " << LTE_BUDGET_MS << " ms)\n"
            << "    Il singolo thread non basta, ma piu' thread in parallelo\n"
            << "    potrebbero compensare (ogni subframe e' indipendente).\n"
            << "    Continuo con il Test 2 per determinare quanti thread servono.\n"
            << "    Se anche il Test 2 fallisce con tutti i thread disponibili,\n"
            << "    prova --prb 15 / --prb 6 oppure --turbo 4.\n"
            << COL_RESET;
        std::cout << "\n";
    }

    // ── TEST 2 ────────────────────────────────────────────────────────────────
    std::cout << "\n" << COL_BOLD
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║ TEST 2: Pipeline depth (quanti MBSFN processor in pool) ║\n"
        << "║  Domanda: quanti job paralleli il pool smaltisce         ║\n"
        << "║  senza accumulare backlog a " << std::setw(5) << interval_ms << " ms/subframe?  ║\n"
        << "╚══════════════════════════════════════════════════════════╝\n"
        << COL_RESET;
    std::cout << "Simulo main loop con 1.." << max_threads
              << " thread, " << nsamples << " subframe @ "
              << interval_ms << " ms/sf\n\n";

    std::cout << COL_BOLD
        << std::setw(9)  << "Threads"
        << std::setw(10) << "Mean(ms)"
        << std::setw(10) << "P99(ms)"
        << std::setw(14) << "Backlog fine"
        << std::setw(12) << "Max queue"
        << std::setw(10) << "Esito"
        << COL_RESET << "\n"
        << std::string(65,'-') << "\n";

    std::vector<PipelineResult> results;
    int recommended = -1;

    for(int t=1; t<=max_threads; t++) {
        std::cout << "  Test " << t << " thread(s)...\r" << std::flush;
        auto r = test2_pipeline(t, p, nsamples, interval_ms);
        results.push_back(r);

        const char* col = r.ok ? COL_GREEN : COL_RED;
        const char* lbl = r.ok ? "OK" : "BACKLOG";
        std::cout << col
            << std::setw(9)  << r.threads
            << std::setw(10) << std::fixed << std::setprecision(2) << r.mean_ms
            << std::setw(10) << r.p99_ms
            << std::setw(14) << r.backlog_at_end
            << std::setw(12) << r.max_queue_depth
            << std::setw(10) << lbl
            << COL_RESET << "\n";

        if(r.ok && recommended == -1) recommended = t;
    }

    // ── Raccomandazione finale ─────────────────────────────────────────────
    std::cout << "\n" << COL_BOLD << "=== RACCOMANDAZIONE FINALE ===" << COL_RESET << "\n\n";

    // Calcola quanti subframe il singolo processor impegna il thread
    // in media → quanti thread servono per coprire il duty cycle
    double duty = s1.mean / interval_ms;
    int theoretical_threads = std::max(1, (int)std::ceil(duty));

    std::cout << "  Latenza media singolo SF:  " << std::setprecision(2) << s1.mean << " ms\n";
    std::cout << "  Intervallo subframe LTE:   " << interval_ms << " ms\n";
    std::cout << "  Duty cycle elaborazione:   " << std::setprecision(0) << duty*100 << "%\n";
    std::cout << "  Thread teorici necessari:  " << theoretical_threads << "\n\n";

    if(recommended != -1) {
        std::cout << COL_GREEN << COL_BOLD
            << "  Imposta nel file di configurazione:\n\n"
            << "    " << COL_CYAN << "modem.phy.threads = " << recommended
            << COL_RESET << COL_GREEN << COL_BOLD << "\n\n"
            << "  (primo valore senza backlog nel Test 2)\n"
            << COL_RESET;
        if(recommended == 1) {
            std::cout << COL_GREEN
                << "\n  Nota: 1 thread e' sufficiente perche' il tuo hardware\n"
                << "  elabora ogni subframe in " << std::setprecision(2) << s1.mean
                << " ms su " << interval_ms << " ms disponibili\n"
                << "  (" << std::setprecision(0) << duty*100 << "% duty cycle)."
                << "  I frame processor aggiuntivi\n"
                << "  non darebbero beneficio e consumerebbero RAM e context switch.\n"
                << COL_RESET;
        }
    } else {
        std::cout << COL_RED << COL_BOLD
            << "  Nessuna configurazione testata regge il carico.\n"
            << "  Prova ad aumentare --max-threads o a ridurre --prb.\n"
            << COL_RESET;
    }
    std::cout << "\n";
    return 0;
}