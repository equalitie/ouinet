#include <atomic>
#include <thread>

#include "client_lib.h"
#include "client.h"

using namespace ouinet;

// One writer, one reader.
// Reader has a copy to make sure that the c_str()'s underlying memory
// is not rug pulled until the next invocation of get_c_str().
class AtomicString {
    std::string master_value;
    std::string read_copy;
    std::atomic_flag read_in_progress;
    std::atomic_flag write_in_progress;
public:
    const char *get_c_str() {
        bool read_lock_acquired = false;
        while (!read_lock_acquired) {
            write_in_progress.wait(true);

            while (read_in_progress.test_and_set()) {
                read_in_progress.wait(true);
            }

            if (write_in_progress.test()) {
                read_in_progress.clear();
                read_in_progress.notify_one();

                std::this_thread::yield();
            } else
                read_lock_acquired = true;
        }

        read_copy = master_value;

        read_in_progress.clear();
        read_in_progress.notify_one();

        return read_copy.c_str();
    }

    const std::string & set(const std::string_view value) {
        write_in_progress.test_and_set();

        read_in_progress.wait(true);

        master_value = value;
        write_in_progress.clear();
        write_in_progress.notify_all();

        return master_value;
    }

    const std::string & set(const std::string_view prefix, const std::string_view value) {
        return set((boost::format("%1%%2%") % prefix % value).str());
    }
};
static AtomicString ouinet_client_error;

const char *ouinet_client_get_error() {
    return ouinet_client_error.get_c_str();
}

// storage for proxy_endpoint, and other string getters
static AtomicString ouinet_client_c_str_storage;

static std::thread g_client_thread;
// g_client is only accessed from the g_client_thread.
static std::unique_ptr<ouinet::Client> g_client;
static asio::io_context g_ctx;

class Destructor_AtomicFlag {
    std::atomic_flag &flag;
    bool did_set{false};
    const bool default_exit_value; // true - set, false - clear
public:
    Destructor_AtomicFlag(std::atomic_flag &flag, const bool default_exit_value):
        flag(flag), default_exit_value(default_exit_value) { }
    ~Destructor_AtomicFlag() {
        if (!did_set) {
            set(default_exit_value);
        }
    }
    void set(const bool value) {
        if (did_set)
            throw std::logic_error("Double set in Destructor_AtomicFlagSet");
        if (value)
            flag.test_and_set();
        else
            flag.clear();
        flag.notify_one();
        did_set = true;
    }
};

class Destructor_CallOnExit {
    void (*callback)(int);
    const int default_exit_value;
    bool did_call = false;
public:
    Destructor_CallOnExit(void (*callback)(int), const int default_exit_value):
        callback(callback), default_exit_value(default_exit_value) { }
    ~Destructor_CallOnExit() {
        if (!did_call) {
            callback(default_exit_value);
        }
    }
    void call(const int argument) {
        if (did_call) {
            throw std::logic_error("Double call in Destructor_CallOnExit");
        }
        callback(argument);
        did_call = true;
    }
};

int ouinet_client_run(const int argc, const char *argv[], void (*on_exit_callback)(int)) {
    if (g_client_thread.joinable()) {
        LOG_ERROR(ouinet_client_error.set("Unexpected ouinet::Client reinitialization"));
        on_exit_callback(EXIT_FAILURE);
        return EXIT_FAILURE;
    }

    // Previous run could be requested to stop without blocking.
    // Need to wait for it to complete.
    static std::atomic_flag ouinet_client_is_already_running {false};
    // this will be cleared inside g_client_thread, on exit
    if (ouinet_client_is_already_running.test_and_set())
        ouinet_client_is_already_running.wait(true);

    ouinet_client_error.set("");

    std::atomic_flag init_was_success;
    std::atomic_flag init_is_complete;

    g_client_thread = std::thread([&] {
        Destructor_AtomicFlag ouinet_client_is_already_running_guard(ouinet_client_is_already_running, false);

        Destructor_AtomicFlag init_is_complete_guard(init_is_complete, true);

        Destructor_CallOnExit on_exit_caller(on_exit_callback, EXIT_FAILURE);

        if (g_client) {
            LOG_ERROR(ouinet_client_error.set("Unexpected ouinet::Client reinitialization"));
            return;
        }

        try {
            static bool crypto_initialized = false;
            if (!crypto_initialized) {
                ouinet::util::crypto_init();
                crypto_initialized = true;
            }

            ClientConfig cfg(argc, argv);
            if (cfg.is_help()) {
                std::stringstream ss;
                ss << "Usage: ouinet_client_run [OPTION...]\n" << cfg.description();
                LOG_INFO(ouinet_client_error.set(ss.str()));
                return;
            }

            LOG_DEBUG("Starting new Ouinet client.");

            // In case we're restarting.
            g_ctx.restart();

            g_client = std::make_unique<ouinet::Client>(g_ctx, std::move(cfg));
            g_client->start();
        } catch (std::exception const &e) {
            LOG_ERROR(ouinet_client_error.set("Error while trying to start Ouinet client: ", e.what()));
            g_client.reset();
            return;
        }

        init_was_success.test_and_set();
        init_is_complete_guard.set(true);

        int retval = EXIT_SUCCESS;

        try {
            g_ctx.run();
        } catch (const std::exception &e) {
            LOG_ERROR(ouinet_client_error.set("Error while running Ouinet client\n", e.what()));
            retval = EXIT_FAILURE;
        }

        LOG_DEBUG("Ouinet's main loop stopped.");
        g_client.reset();
        on_exit_caller.call(retval);
    });
    init_is_complete.wait(false);
    if (!init_was_success.test()) {
        g_client_thread.join();
        g_client_thread = std::thread();
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void ouinet_client_stop_and_detach() {
    if (!g_client_thread.joinable())
        return;

    asio::post(g_ctx, [] {
        try {
            HandlerTracker::stopped();
            if (g_client) {
                g_client->stop();
            }
        } catch (const std::exception &e) {
            LOG_ERROR(ouinet_client_error.set("Failed to stop Ouinet client: ", e.what()));
        }
    });
    g_client_thread.detach();
    g_client_thread = std::thread();
}

void ouinet_client_stop_and_wait_for_completion() {
    if (!g_client_thread.joinable())
        return;

    asio::post(g_ctx, [] {
        try {
            HandlerTracker::stopped();
            if (g_client) {
                g_client->stop();
            }
        } catch (const std::exception &e) {
            LOG_ERROR(ouinet_client_error.set("Failed to stop Ouinet client: ", e.what()));
        }
    });
    g_client_thread.join();
    g_client_thread = std::thread();
}

int ouinet_client_get_client_state() {
    if (!g_client_thread.joinable())
        return g_ctx.stopped() ? 6 /* stopped */ : 0 /* created */;

    std::atomic_int state {-1};
    std::atomic_flag value_is_known;
    asio::post(g_ctx, [&state, &value_is_known] {
        switch (g_client->get_state()) {
            // TODO: Avoid needing to keep this in sync by hand.
            case ouinet::Client::RunningState::Created: state = 0;
                break;
            case ouinet::Client::RunningState::Failed: state = 1;
                break;
            case ouinet::Client::RunningState::Starting: state = 2;
                break;
            case ouinet::Client::RunningState::Degraded: state = 3;
                break;
            case ouinet::Client::RunningState::Started: state = 4;
                break;
            case ouinet::Client::RunningState::Stopping: state = 5;
                break;
            case ouinet::Client::RunningState::Stopped: state = 6;
                break;
        }
        value_is_known.test_and_set();
        value_is_known.notify_one();
    });
    value_is_known.wait(false);
    return state;
}

const char *ouinet_client_get_proxy_endpoint() {
    if (!g_client_thread.joinable())
        return "";

    std::atomic_flag value_is_known;
    asio::post(g_ctx, [&value_is_known] {
        if (g_client) {
            const auto ep = g_client->get_proxy_endpoint();
            ouinet_client_c_str_storage.set((boost::format("%1%:%2%") % ep.address().to_string() % ep.port()).str());
        } else
            ouinet_client_c_str_storage.set("");
        value_is_known.test_and_set();
        value_is_known.notify_one();
    });
    value_is_known.wait(false);
    return ouinet_client_c_str_storage.get_c_str();
}

const char *ouinet_client_get_frontend_endpoint() {
    if (!g_client_thread.joinable())
        return "";

    std::atomic_flag value_is_known;
    asio::post(g_ctx, [&value_is_known] {
        if (g_client)
            ouinet_client_c_str_storage.set(g_client->get_frontend_endpoint());
        else
            ouinet_client_c_str_storage.set("");
        value_is_known.test_and_set();
        value_is_known.notify_one();
    });
    value_is_known.wait(false);
    return ouinet_client_c_str_storage.get_c_str();
}
