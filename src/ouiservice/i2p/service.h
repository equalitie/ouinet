#pragma once

#include "namespaces.h"
#include "api.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem/path.hpp>

#include <memory>
#include <variant>
#include <expected>

namespace ouinet {

class Async;
class Cancel;
class I2pSession;
namespace util { class LogPath; }

//
// Ensures I2P is running by (in this order):
//
// 1. Trying to connect to an existing I2P service running outside of Ouinet if
//    * `external_endpoint` is set in the config
// 2. If that fails, trying to spawn `i2pd` as child process if
//    * The OS supports child processes (iOS doesn't)
//    * Ouinet was compiled with `WITH_I2PD_EXE=ON`
//    * `i2p_exe_path` is set in the config
// 3. Trying to start `i2pd` in a separate thread, if
//    * Ouinet was compiled with `WITH_I2PD_LIB=ON`
//    * `use_i2p_lib` is `true` in the config
//
// While the service exists, it performs periodic health checks and attempts to
// restart the service if it fails.
//

class OUINET_I2P_API I2pService {
public:
    struct ConfigExternal {
        asio::ip::tcp::endpoint endpoint;
    };

    struct ConfigI2pdExe {
        fs::path i2pd_exe_path;
        fs::path datadir;
    };

    struct ConfigI2pdLib {
        fs::path datadir;
    };

    struct Config {
        std::optional<ConfigExternal> ext;
        std::optional<ConfigI2pdExe> i2pd_exe;
        std::optional<ConfigI2pdLib> i2pd_lib;
    };

    struct State {
        struct Starting {};
        struct PerformingHealthCheck {};
        struct Running {
            asio::ip::tcp::endpoint sam_endpoint;
        };
        struct Aborted {};

        using Alternatives = std::variant<
            Starting,
            PerformingHealthCheck,
            Running,
            Aborted
        >;

        template<class V>
        requires(!std::is_same_v<V, State> && std::constructible_from<Alternatives, V>)
        State(V&& v) : value(std::forward<V>(v)) {}

        Alternatives value;

        template<class S> const S* as() const {
            return std::get_if<S>(&value);
        }

        template<class S> bool is() const {
            return as<S>() != nullptr;
        }
    };

    static I2pService start(Config, asio::any_io_executor, Cancel, util::LogPath);

    State get_state() const;

    // Returns `State::Running` when the state is `Running` and `none` if no
    // more attempts to start the service will be made (the state is
    // `Aborted`). Note that even if state is `Running` it can later change if
    // the connection to the service is lost.
    std::optional<State::Running> await_running_state(Async) const;

    std::expected<I2pSession, sys::error_code> create_session(Async);

private:
    struct Inner;

    I2pService(std::shared_ptr<Inner> inner) : _inner(std::move(inner)) {}
    std::shared_ptr<Inner> _inner;
};

} // namespace
