#ifndef DATAVIEWER_H
    #define DATAVIEWER_H

    #include <thread>
    #include <chrono>
    #include <vector>
    #include <map>
    #include <set>
    #include <string>
    #include <mutex>
    #include <condition_variable>
    #include <iostream>
    
    #include <stdint.h>

    #ifndef WIN32
        #include <sys/ioctl.h>
        #include <unistd.h>
    #else
        #include <windows.h>
    #endif

    namespace dvw {

        /**
         * Logger Thread.
         */
        const std::thread* dvw_thread() noexcept;
        
        /**
         * Funzione per iniziare il log; utilizzabile solo quando il logger non
         * è mai stato attivato oppure dopo aver chiamato `stop()`.
         */
        void start() noexcept;
        /**
         * Interrompe il logging delle informazioni. Utilizzabile solo
         * a logger attivo.
         */
        void stop() noexcept;

        /**
         * Arresta momentaneamente il log delle attività liberando il terminale;
         * utilizzabile solo quando il logger è attivo e non in pausa.
         * Si annulla chiamando `resume()`.
         */
        void pause() noexcept;
        /**
         * Riprende l'attività di logging dopo aver chiamato `pause()`; utilizzabile
         * solo a logger attivo ed in pausa.
         */
        void resume() noexcept;

        /**
         * Imposta un tempo di aggiornamento del logger in nanosecondi.
         */
        void upd_nanoseconds(uint64_t ns) noexcept;
        /**
         * Imposta un tempo di aggiornamento del logger in millisecondi.
         */
        void upd_milliseconds(uint64_t ms) noexcept;
        /**
         * Imposta un tempo di aggiornamento del logger in secondi.
         */
        void upd_seconds(uint64_t s) noexcept;

        /**
         * Implementa una label nel logger che corrisponde a `[ label : messaggio ]`,
         * il valore si aggiorna una volta per update.
         */
        void log(const std::string& label, const std::string& msg) noexcept;
        /**
         * Implementa una label nel logger che corrisponde a `[ label : numero_intero ]`,
         * il valore si aggiorna una volta per update.
         */
        void log_i(const std::string& label, int64_t l) noexcept;
        /**
         * Implementa una label nel logger che corrisponde a `[ label : floating_point ]`,
         * il valore si aggiorna una volta per update.
         */
        void log_f(const std::string& label, double d) noexcept;

        /**
         * Rimuove dal logger la label `[ label : ... ]`. Tentare di loggare nuovamente
         * la stessa label non produrrà risultato fino alla chiamata di `forgive(label)`.
         */
        void forget(const std::string& label) noexcept;
        /**
         * Dopo aver ignorato una label con `forget(label)`, notifica il logger che questa
         * è nuovamente ritenuta necessaria da monitorare.
         */
        void forgive(const std::string& label) noexcept;
    }

#endif