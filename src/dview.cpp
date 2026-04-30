#include "dview.hpp"

namespace dvw {

    static std::thread* DVW_THREAD = nullptr;

    static bool IS_UP = false;
    static bool IS_PAUSED = false;

    static uint32_t WINDOW_SIZE;
    
    #ifndef WIN32
        #define CLEAR "clear"

        static void get_window_size() {
            struct winsize w;
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
            WINDOW_SIZE = w.ws_row;
        }
    #else
        #define CLEAR "cls"

        static void get_window_size() {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
            WINDOW_SIZE = csbi.srWindow.Bottom - csbi.srWindow.Top;
        }
    #endif

    const std::thread* dvw_thread() noexcept {
        return DVW_THREAD;
    }

    static std::set<std::string> FORGOTTEN;
    static std::map<std::string, std::string> LOGGER;
    static std::vector<std::pair<std::string, std::string>> SNAPSHOT;

    static std::mutex MUTEX;

    static uint64_t UPDATE_TIME = 1e9;

    void upd_nanoseconds(uint64_t ns) noexcept {
        UPDATE_TIME = ns;
    }

    void upd_milliseconds(uint64_t ms) noexcept {
        UPDATE_TIME = ms * 1e6;
    }

    void upd_seconds(uint64_t s) noexcept {
        UPDATE_TIME = s * 1e9;
    }

    static void print_frame() noexcept {
        std::string frame;

        for (uint32_t i = 0, e = 0; i < WINDOW_SIZE; i++, e++) {
            if (e < SNAPSHOT.size()) {
                uint32_t a = e;
                do {
                    frame += "[ " + (*(SNAPSHOT.begin()+a)).first + " : " + (*(SNAPSHOT.begin()+a)).second + " ] ";
                }
                while ((a += (WINDOW_SIZE-2)) < SNAPSHOT.size());
                frame += "\n";
            }
            else if (i != WINDOW_SIZE-1) {
                frame += "\n";
            }
        }

        std::cout << frame << std::endl;
    }

    static void update() noexcept {
        LOGGER.clear();
        while (IS_UP) {
            if (!IS_PAUSED) {
                system(CLEAR);
                get_window_size();

                {
                    std::lock_guard<std::mutex> lock(MUTEX);
                    SNAPSHOT.assign(LOGGER.begin(), LOGGER.end());
                }

                print_frame();
            }

            std::this_thread::sleep_for(std::chrono::nanoseconds(UPDATE_TIME));
        }
        system(CLEAR);
    }

    void start() noexcept {
        if (DVW_THREAD == nullptr) {
            IS_UP = true;
            get_window_size();
            DVW_THREAD = new std::thread(&update);
        }
    }

    void stop() noexcept {
        if (DVW_THREAD != nullptr) {
            IS_UP = false;
            DVW_THREAD->join();
            delete DVW_THREAD;
            DVW_THREAD = nullptr;
        }
    }

    void pause() noexcept {
        std::lock_guard<std::mutex> lock(MUTEX);
        if (DVW_THREAD != nullptr && !IS_PAUSED) {
            IS_PAUSED = true;
            std::cout << std::endl;
            system(CLEAR);
        }
    }

    void resume() noexcept {
        std::lock_guard<std::mutex> lock(MUTEX);
        if (DVW_THREAD != nullptr && IS_PAUSED) {
            IS_PAUSED = false;
        }
    }

    void log(const std::string& label, const std::string& msg) noexcept {
        std::lock_guard<std::mutex> lock(MUTEX);
        if (!FORGOTTEN.count(label)) {
            LOGGER[label] = msg;
        }
    }

    void log_i(const std::string& label, int64_t i) noexcept {
        log(label, std::to_string(i));
    }

    void log_f(const std::string& label, double d) noexcept {
        log(label, std::to_string(d));
    }

    void forget(const std::string& label) noexcept {
        std::lock_guard<std::mutex> lock(MUTEX);
        if (LOGGER.count(label) != 0) {
            FORGOTTEN.insert(label);
            LOGGER.erase(label);
        }
    }

    void forgive(const std::string& label) noexcept {
        std::lock_guard<std::mutex> lock(MUTEX);
        if (FORGOTTEN.count(label) != 0) {
            FORGOTTEN.erase(label);
        }
    }
}