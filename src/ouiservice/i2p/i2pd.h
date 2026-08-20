#pragma once

#include "namespaces.h"
#include "api.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem/path.hpp>
#include <expected>
#include <vector>
#include <string>
#include <variant>

namespace ouinet {

class Async;
namespace util { class LogPath; }

// Class for controlling the `i2pd` executable/library
class OUINET_I2P_API I2pd {
public:
    struct Type {
        struct Exe {};
        struct Lib {};

        using Alternatives = std::variant<Exe, Lib>;

        template<class V>
        requires(!std::is_same_v<V, Type> && std::constructible_from<Alternatives, V>)
        Type(V&& v) : value(std::forward<V>(v)) {}

        template<class Visitor, class Self>
        decltype(auto) visit(this Self&& self, Visitor&& visitor) {
            return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).value);
        }

        Alternatives value;
    };

    class Config {
    public:
        Config(fs::path i2pd_root_dir):
            i2pd_root_dir(std::move(i2pd_root_dir))
        {}
    
        std::vector<std::string> to_vector(Type) const;
    
    private:
        // TODO: Bind to a random port (0)
        uint16_t sam_port = 7656;
        //uint16_t sam_port = 0;
        fs::path i2pd_root_dir;
    };

    // If these return `false` then the corresponding `start_*` functions
    // declared below will return immediatelly with an error.
    static bool is_start_lib_implemented();
    static bool is_start_exe_implemented();

    // Start i2pd using an external executable in a new process.
    [[nodiscard]]
    static
    std::expected<I2pd, sys::error_code>
    start_exe(fs::path i2pd_binary_path, Config, Async);

    // Start i2pd through i2pd library compiled into ouinet.
    [[nodiscard]]
    static
    std::expected<I2pd, sys::error_code>
    start_lib(Config, util::LogPath);

    // Endpoint of the SAM bridge to which we can connect.
    asio::ip::tcp::endpoint sam_endpoint() const;

private:
    struct InnerLib;
    struct InnerExe;
    struct InnerBase {
        virtual asio::ip::tcp::endpoint sam_endpoint() const = 0;
        virtual ~InnerBase() = default;
    };

    I2pd(std::unique_ptr<InnerBase> inner) : _inner(std::move(inner)) {}

    std::unique_ptr<InnerBase> _inner;
};

} // namespace
