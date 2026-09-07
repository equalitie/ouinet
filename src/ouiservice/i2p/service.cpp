#include "service.h"
#include "util/async.h"
#include "util/spawn_for_result.h"
#include "util/log_path.h"
#include "util/str.h"
#include "util/watch.h"
#include "util/select.h"
#include "util/overloaded.h"
#include "logger.h"
#include "async_sleep.h"
#include "session.h"
#include "address.h"
#include "i2pd.h"
#include "destination_keypair.h"

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

struct Init {
    struct External {
        tcp::endpoint sam_endpoint;
        I2pSession session;
    };

    struct Internal {
        I2pd i2pd;
        I2pSession session;
    };

    I2pSession& session() {
        return std::visit(overloaded {
                [] (External& v) -> auto& { return v.session; },
                [] (Internal& v) -> auto& { return v.session; }
            },
            value);
    }

    tcp::endpoint sam_endpoint() const {
        return std::visit(overloaded {
                [] (External const& v) { return v.sam_endpoint; },
                [] (Internal const& v) { return v.i2pd.sam_endpoint(); }
            },
            value);
    }

    using Alternatives = std::variant<External, Internal>;

    template<class V>
    requires(!std::is_same_v<V, Init> && std::constructible_from<Alternatives, V>)
    Init(V&& v) : value(std::forward<V>(v)) {}

    Alternatives value;
};

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

    std::expected<I2pSession, sys::error_code>
    create_i2p_session_with_retry(tcp::endpoint sam_endpoint, Async yield) {
        using R = std::expected<I2pSession, sys::error_code>;

        auto now = std::chrono::steady_clock::now;

        sys::error_code last_error = asio::error::fault;

        auto max_duration = 2min;
        auto start = now();
        auto end = start + max_duration;

        while (true) {
            auto iter_start = now();

            if (end <= iter_start) {
                break;
            }

            auto session = timeout(end - iter_start, [&] (Async yield) -> R {
                    return I2pSession::create(sam_endpoint, yield);
                },
                yield);

            if (session) {
                return session;
            }

            last_error = session.error();

            auto actual_duration = now() - iter_start;

            if (actual_duration < 1s) {
                async_sleep(1s - actual_duration, yield);
            }
        }

        return std::unexpected(last_error);
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

        auto server_socket = std::move(server_task.wait_ref(yield));

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

    void keep_performing_health_check(Init init, Async yield) {
        LOG_DEBUG(yield, " Creating session 2/2");
        state.send(State::PerformingHealthCheck{});

        auto session0 = std::move(init.session());
        auto session1 = I2pSession::create(init.sam_endpoint(), yield);

        if (!session1) {
            LOG_DEBUG(yield, " Session 2 creation failed: ", session1.error());
            return;
        }

        LOG_DEBUG(yield, " Performing self connection check");

        auto connections = connected_pair(
                std::move(session0),
                std::move(*session1),
                yield);

        if (!connections) {
            LOG_DEBUG(yield, " Health check failed in self connection: ", connections.error());
            return;
        }

        uint64_t n = 0;
        while (ping(connections->first.socket, connections->second.socket, n, yield)) {
            if (!state.value().is<State::Running>()) {
                LOG_DEBUG(yield, " Self connection passed. SAM endpoint: ", init.sam_endpoint());
                state.send(State::Running{ init.sam_endpoint() });
            }
            ++n;
            async_sleep(10s, yield);
        }

        LOG_DEBUG(yield, " Health check failed in ping");
        state.send(State::Starting{});
    }

    std::optional<Init> try_external_service(Async yield) {
        auto conf = config.ext;

        if (!conf) {
            LOG_DEBUG(yield, " Not configured");
            return {};
        }

        LOG_DEBUG(yield, " Creating session 1/2 ");

        auto session = create_i2p_session_with_retry(conf->endpoint, yield);

        if (!session) {
            LOG_DEBUG(yield, " Session 1 creation failed: ", session.error());
            return {};
        }

        return Init::External { conf->endpoint, std::move(*session) };
    }

    std::optional<Init> try_i2pd_exe(Async yield) {
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
            LOG_DEBUG(yield, " Failed to start i2pd: ", i2pd.error());
            return {};
        }

        LOG_DEBUG(yield, " Creating session 1/2");

        auto session = create_i2p_session_with_retry(i2pd->sam_endpoint(), yield);

        if (!session) {
            LOG_DEBUG(yield, " Session 1 creation failed: ", session.error());
            return {};
        }

        return Init::Internal { std::move(*i2pd), std::move(*session) };
    }

    std::optional<Init> try_i2pd_lib(Async yield) {
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
            LOG_DEBUG(yield, " Failed to start i2pd: ", i2pd.error());
            return {};
        }

        LOG_DEBUG(yield, " Creating session 1/2");

        auto session = create_i2p_session_with_retry(i2pd->sam_endpoint(), yield);

        if (!session) {
            LOG_DEBUG(yield, " Session 1 creation failed: ", session.error());
            return {};
        }

        return Init::Internal { std::move(*i2pd), std::move(*session) };
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

            if (auto init = try_external_service(yield.tag("ext"))) {
                keep_performing_health_check(std::move(*init), yield.tag("ext"));
            }
            else if (auto init = try_i2pd_exe(yield.tag("exe"))) {
                keep_performing_health_check(std::move(*init), yield.tag("exe"));
            }
            else if (auto init = try_i2pd_lib(yield.tag("lib"))) {
                keep_performing_health_check(std::move(*init), yield.tag("lib"));
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
    
    return I2pSession::create(running_state->sam_endpoint, yield);
}

std::expected<I2pSession, sys::error_code> I2pService::create_session(I2pDestinationKeypair keypair, Async yield) {
    auto running_state = await_running_state(yield);
    
    if (!running_state) {
        return std::unexpected(asio::error::service_not_found);
    }
    
    auto sam = Sam::connect(running_state->sam_endpoint, yield);
    if (!sam) return std::unexpected(sam.error());

    return I2pSession::create(std::move(*sam), std::move(keypair), yield);
}

} // namespace
