#pragma once

#include "error.h"
#include "namespaces.h"
#include "api.h"

#include <boost/asio/ip/tcp.hpp>

#include <optional>
#include <variant>
#include <expected>
#include <ostream>
#include <string>
#include <string_view>

namespace ouinet {

struct SessionId;
class Async;
class I2pAddress;

// https://i2p.net/en/docs/api/samv3/
struct OUINET_I2P_API Sam {
public:
    struct Keypair {
        std::string pub;
        std::string priv;

        std::string to_json_string();
        std::optional<Keypair> from_json_string(std::string_view);
    };

    struct Error {
        struct IoConnect {
            sys::error_code ec;
            friend std::ostream& operator<<(std::ostream& os, const IoConnect& e) {
                return os << "IoConnect{" << e.ec.message() << "}";
            }
            operator sys::error_code() const { return ec; }
        };
        struct IoSend {
            sys::error_code ec;
            friend std::ostream& operator<<(std::ostream& os, const IoSend& e) {
                return os << "IoSend{ " << e.ec.message() << " }";
            }
            operator sys::error_code() const { return ec; }
        };
        struct IoRecv {
            sys::error_code ec;
            friend std::ostream& operator<<(std::ostream& os, const IoRecv& e) {
                return os << "IoRecv{ " << e.ec.message() << " }";
            }
            operator sys::error_code() const { return ec; }
        };
        struct Invoke {
            using Value = std::variant<IoSend, IoRecv>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Invoke& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Invoke{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Result {
            struct NoVersion {
                friend std::ostream& operator<<(std::ostream& os, const NoVersion&) {
                    return os << "NOVERSION";
                }
                operator sys::error_code() const {
                    return OuinetError::i2p;
                }
            };
            struct DuplicatedId {
                friend std::ostream& operator<<(std::ostream& os, const DuplicatedId&) {
                    return os << "DuplicatedId";
                }
                operator sys::error_code() const {
                    return OuinetError::i2p;
                }
            };
            struct DuplicatedDest {
                friend std::ostream& operator<<(std::ostream& os, const DuplicatedDest&) {
                    return os << "DuplicatedDest";
                }
                operator sys::error_code() const {
                    return OuinetError::i2p;
                }
            };
            struct InvalidKey {
                friend std::ostream& operator<<(std::ostream& os, const InvalidKey&) {
                    return os << "InvalidKey";
                }
                operator sys::error_code() const {
                    return OuinetError::i2p;
                }
            };
            struct I2pError {
                std::string message;
                friend std::ostream& operator<<(std::ostream& os, const I2pError& e) {
                    return os << "I2P_ERROR{ " << e.message << " }";
                }
                operator sys::error_code() const {
                    return OuinetError::i2p;
                }
            };
        };
        // Failed to parse I2pAddress
        struct InvalidAddress {
            std::string input;
            friend std::ostream& operator<<(std::ostream& os, const InvalidAddress& e) {
                return os << "InvalidAddress{ \"" << e.input << "\" }";
            }
            operator sys::error_code() const {
                return asio::error::invalid_argument;
            }
        };
        struct UnexpectedResponse {
            std::string request;
            std::string response;
            friend std::ostream& operator<<(std::ostream& os, const UnexpectedResponse& e) {
                return os << "UnexpectedResponse{ request: \"" << e.request << "\" response: \"" << e.response << "\" }";
            }
            operator sys::error_code() const { return OuinetError::i2p; }
        };
        struct Handshake {
            using Value = std::variant<Invoke, UnexpectedResponse, Result::NoVersion, Result::I2pError>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Handshake& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Handshake{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Connect {
            using Value = std::variant<IoConnect, Handshake>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Connect& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Connect{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct DestGenerate {
            using Value = std::variant<Invoke, UnexpectedResponse, InvalidAddress>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const DestGenerate& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "DestGenerate{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct CreateSession {
            using Value = std::variant<
                Invoke,
                UnexpectedResponse,
                Result::DuplicatedId,
                Result::DuplicatedDest,
                Result::InvalidKey,
                Result::I2pError,
                InvalidAddress
            >;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const CreateSession& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "CreateSession{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Lookup {
            using Value = std::variant<Invoke, Result::InvalidKey, UnexpectedResponse, InvalidAddress>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Lookup& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Lookup{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Ping {
            using Value = std::variant<Invoke, UnexpectedResponse>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Ping& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Ping{ " << e << " }"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        template<class Eo> static auto wrap() { return [](auto ei) { return Eo(std::move(ei)); }; }
    };

    static asio::ip::tcp::endpoint default_endpoint() {
        return asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7656);
    }

    Sam(Sam&&);

    // Connect to TCP socket and handshake
    [[nodiscard]]
    static
    std::expected<Sam, Error::Connect> connect(asio::ip::tcp::endpoint, Async);

    [[nodiscard]]
    std::expected<void, Error::IoSend> send_line(const std::string& line, Async);

    [[nodiscard]]
    std::expected<std::string, Error::IoRecv> recv_line(Async);

    [[nodiscard]]
    std::expected<std::string, Error::Invoke> invoke(const std::string& request, Async);

    [[nodiscard]]
    std::expected<I2pAddress, Error::CreateSession> create_session(SessionId const&, const Keypair&, Async);

    [[nodiscard]]
    std::expected<void, Error::Handshake> handshake(Async);

    // `name` may be a "*.b32.i2p" address or a domain name (e.g. "example.i2p").
    // Note: This lookup should also support getting our own b64 address, but that wasn't
    // needed at the time of implementing it.
    [[nodiscard]]
    std::expected<std::optional<I2pAddress>, Error::Lookup> lookup(const std::string& name, Async);

    [[nodiscard]]
    std::expected<std::string, Error::Ping> ping(const std::string& msg, Async);

    asio::ip::tcp::socket release_socket();

    asio::any_io_executor get_executor();

    bool is_open() const;

    void close();

    // Remote endpoint of the socket communicating with the I2P service.
    asio::ip::tcp::endpoint remote_endpoint() const;

    ~Sam();

    [[nodiscard]]
    std::expected<Keypair, Error::DestGenerate> dest_generate(Async);

private:
    // Only `Sam::connect` may construct a Sam. This way it ensures the socket is
    // open and connected before handing it in. Because Inner constructor
    // relies on the socket being open when caching the remote endpoint.
    Sam(asio::ip::tcp::socket socket);

private:
    struct Inner;
    std::unique_ptr<Inner> _inner;
};

} // ouinet namespace
