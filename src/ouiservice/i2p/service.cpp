#include "service.h"
#include "util/async.h"
#include "util/spawn_for_result.h"
#include "util/log_path.h"
#include "util/str.h"
#include "util/watch.h"
#include "util/select.h"
#include "logger.h"
#include "async_sleep.h"
#include "session.h"
#include "address.h"
#include "i2pd.h"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

namespace ouinet {

using State = I2pService::State;
using namespace std::chrono_literals;
using tcp = asio::ip::tcp;

struct Connection {
    I2pSession session;
    tcp::socket socket;
};

struct ServiceExternal { tcp::endpoint sam_endpoint; };

struct I2pService::Inner {
    Config config;
    LifetimeCancel cancel;
    TaskHandle<void> task;
    Watch<State>::Producer state;

    Inner(Config config, asio::any_io_executor exec) :
        config(std::move(config)),
        state(exec, State::Starting{})
    {}

    Watch<State> get_state_watch() const {
        return Watch<State>(state);
    }

    std::expected<
        std::pair<
            I2pSession,
            I2pSession
        >,
        sys::error_code
    >
    create_two_sessions(tcp::endpoint sam_endpoint, Async yield) {
        auto session0 = I2pSession::create(yield, sam_endpoint);
        if (!session0) return std::unexpected(session0.error());

        auto session1 = I2pSession::create(yield, sam_endpoint);
        if (!session1) return std::unexpected(session1.error());

        return std::pair(std::move(*session0), std::move(*session1));
    }

    std::expected<
        std::pair<
            Connection,
            Connection
        >,
        sys::error_code
    >
    connected_pair(I2pSession client_session, I2pSession server_session, Async yield) {
        using R = std::expected<tcp::socket, sys::error_code>;

        auto addr = server_session.local_addr().to_b32();

        WaitCondition wc(yield.get_executor());

        auto server_task = spawn_for_result(yield.get_executor(), yield.get_cancel(), yield.log_path(), [&] (Async yield) {
            return timeout(60s, [&] (Async yield) -> R { return server_session.accept(yield); }, yield);
        });

        auto client_socket = timeout(60s, [&] (Async yield) -> R { return client_session.connect(addr, yield); }, yield);

        if (!client_socket) {
            server_task.cancel();
        }

        auto server_socket = timeout(10s, [&] (Async yield) -> R {
                return std::move(server_task.wait_ref(yield));
            }, yield);

        if (!client_socket) return std::unexpected(client_socket.error());
        if (!server_socket) return std::unexpected(server_socket.error());

        return std::pair(
            Connection { std::move(client_session), std::move(*client_socket) },
            Connection { std::move(server_session), std::move(*server_socket) }
        );
    }

    bool ping(tcp::socket& s0, tcp::socket& s1, uint64_t n, Async yield) {
        auto wr = asio::async_write(s0, asio::buffer((void*) &n, sizeof(n)), yield);
        if (!wr) return false;
        decltype(n) n_received;
        auto rr = asio::async_read(s1, asio::buffer((void*) &n_received, sizeof(n_received)), yield);
        if (!rr) return false;
        return n == n_received;
    }

    void keep_performing_health_check(tcp::endpoint sam_endpoint, Async yield) {
        LOG_DEBUG(yield, " Trying to connect to the service");
        state.send(State::PerformingHealthCheck{});

        std::expected<std::pair<I2pSession, I2pSession>, sys::error_code> sessions
            = std::unexpected(asio::error::fault);

        for (unsigned i = 0; i < 10; ++i) {
            sessions = create_two_sessions(sam_endpoint, yield);
            if (!sessions) {
                async_sleep(500ms, yield);
            }
            else {
                break;
            }
        }

        if (!sessions) {
            LOG_DEBUG(yield, " Connecting to service failed: ", sessions.error());
            return;
        }

        LOG_DEBUG(yield, " Performing self connection check");

        auto connections = connected_pair(
                std::move(sessions->first),
                std::move(sessions->second),
                yield);

        if (!connections) {
            LOG_DEBUG(yield, " Health check failed in self connection: ", connections.error());
            return;
        }

        uint64_t n = 0;
        while (ping(connections->first.socket, connections->second.socket, n, yield)) {
            if (!state.value().is<State::Running>()) {
                LOG_DEBUG(yield, " Health check passed, will continue pinging health check");
                state.send(State::Running{ sam_endpoint });
            }
            ++n;
            async_sleep(10s, yield);
        }

        LOG_DEBUG(yield, " Health check failed in ping");
        state.send(State::Starting{});
    }

    std::optional<ServiceExternal> try_external_service(Async yield) {
        auto conf = config.ext;

        if (!conf) {
            LOG_DEBUG(yield, " Not configured");
            return {};
        }

        LOG_DEBUG(yield, " Starting ", conf->endpoint);

        auto session = I2pSession::create(yield, conf->endpoint);

        if (!session) {
            LOG_DEBUG(yield, " Failed: ", session.error());
            return {};
        }

        LOG_DEBUG(yield, " Success ", conf->endpoint);

        return ServiceExternal { conf->endpoint };
    }

    std::optional<I2pd> try_i2pd_exe(Async yield) {
        auto& conf = config.i2pd_exe;

        if (!conf) {
            LOG_DEBUG(yield, " Not configured");
            return {};
        }

        if (!I2pd::is_start_exe_implemented()) {
            LOG_DEBUG(yield, " Not compiled with WITH_I2PD_EXE=ON");
            return {};
        }

        LOG_DEBUG(yield, " Starting ", conf->i2pd_exe_path);
        auto i2pd = I2pd::start_exe(
            conf->i2pd_exe_path,
            I2pd::Config(conf->datadir),
            yield
        );

        if (!i2pd) {
            LOG_DEBUG(yield, " Failed: ", i2pd.error());
            return {};
        }

        LOG_DEBUG(yield, " Success ", i2pd->sam_endpoint());
        return std::move(*i2pd);
    }

    std::optional<I2pd> try_i2pd_lib(Async yield) {
        auto& conf = config.i2pd_lib;

        if (!conf) {
            LOG_DEBUG(yield, " Not configured");
            return {};
        }

        if (!I2pd::is_start_lib_implemented()) {
            LOG_DEBUG(yield, " Not compiled with WITH_I2PD_LIB=ON");
            return {};
        }

        LOG_DEBUG(yield, " Starting");
        auto i2pd = I2pd::start_lib(
            I2pd::Config(conf->datadir),
            yield.log_path()
        );

        if (!i2pd) {
            LOG_DEBUG(yield, " Failed: ", i2pd.error());
            return {};
        }

        LOG_DEBUG(yield, " Success ", i2pd->sam_endpoint());

        return std::move(*i2pd);
    }

    void run(Async yield) {
        auto slot = cancel.connect([&] { yield.cancel(); });

        auto delay = 3s;

        while (true) {
            size_t option_count =
                (config.ext ? 1 : 0) +
                (config.i2pd_exe && I2pd::is_start_exe_implemented() ? 1 : 0) +
                (config.i2pd_lib && I2pd::is_start_lib_implemented() ? 1 : 0);

            if (option_count == 0) {
                LOG_WARN(yield, " No I2P service to try, aborting");
                state.send(State::Aborted{});
                return;
            }

            if (auto s = try_external_service(yield.tag("ext"))) {
                keep_performing_health_check(s->sam_endpoint, yield.tag("ext"));
            }
            else if (auto s = try_i2pd_exe(yield.tag("exe"))) {
                keep_performing_health_check(s->sam_endpoint(), yield.tag("exe"));
            }
            else if (auto s = try_i2pd_lib(yield.tag("lib"))) {
                keep_performing_health_check(s->sam_endpoint(), yield.tag("lib"));
            }

            async_sleep(delay, yield);
        }
    }
};

/* static */
I2pService I2pService::start(Config config, asio::any_io_executor exec, Cancel cancel, util::LogPath log_path) {
    auto inner = std::make_shared<Inner>(std::move(config), exec);

    inner->task = spawn_for_result(exec, cancel, log_path, [inner = inner.get()] (Async yield) {
            if (yield.is_cancelled()) return;
            inner->run(yield.tag("I2pService"));
        });

    return I2pService(std::move(inner));
}

std::optional<State::Running> I2pService::await_running_state(Async yield) const {
    Watch<State> watch(_inner->state);

    while (watch.await_change(yield)) {
        if (auto running_state = _inner->state.value().as<State::Running>()) {
            return *running_state;
        }
    }

    return {};
}

State I2pService::get_state() const {
    return _inner->state.value();
}

std::expected<I2pSession, sys::error_code> I2pService::create_session(Async yield) {
    auto running_state = await_running_state(yield);
    
    if (!running_state) {
        return std::unexpected(asio::error::service_not_found);
    }
    
    return I2pSession::create(yield, running_state->sam_endpoint);
}

} // namespace
