#pragma once

#include "sam.h"
#include "../../util/cancel.h"
#include "api.h"

#include <variant>
#include <ostream>

namespace ouinet {

class Async;

class OUINET_I2P_API I2pSession {
public:
    struct Error {
        struct Create {
            using Value = std::variant<Sam::Error::Connect, Sam::Error::CreateSession, Sam::Error::DestGenerate>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Create& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Create {" << e << "}"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Connect {
            using Value = std::variant<Sam::Error::Connect, Sam::Error::Invoke>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Connect& e) {
                return std::visit([&os] (auto& e) -> auto&
                    { return os << "Connect {" << e << "}"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
        struct Accept {
            using Value = std::variant<Sam::Error::Connect, Sam::Error::Invoke>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Accept& e) {
                return std::visit([&os] (auto& e) -> auto&
                    { return os << "Accept {" << e << "}"; },
                    e.value);
            }
            operator sys::error_code() const {
                return std::visit([] (auto& e) -> sys::error_code { return e; }, value);
            }
        };
    };

    I2pSession(I2pSession&&) = default;
    I2pSession& operator=(I2pSession&&) = default;

    [[nodiscard]]
    static std::expected<I2pSession, Error::Create> create(Sam, I2pDestinationKeypair, Async);

    // Conveniece to the above: connects to `Sam` and auto generates `I2pDestinationKeypair`.
    [[nodiscard]]
    static std::expected<I2pSession, Error::Create> create(asio::ip::tcp::endpoint sam_ep, Async);

    [[nodiscard]]
    std::expected<asio::ip::tcp::socket, Error::Connect> connect(const I2pAddress& remote_addr, Async);

    // NOTE: Cancelling this operation will likely cause the next connect to
    // `_local_addr` to succeed, but the socket will immediatelly be closed by
    // I2P (observed with the Java I2P implementation).
    [[nodiscard]]
    std::expected<asio::ip::tcp::socket, Error::Accept> accept(Async);

    [[nodiscard]]
    std::expected<std::optional<I2pAddress>, Sam::Error::Lookup> lookup(const std::string& name, Async);

    const I2pAddress& local_addr() const;

    const I2pDestinationKeypair& destination_keypair() const;

    asio::any_io_executor get_executor();

    bool is_open() const;

    ~I2pSession();

private:
    struct Inner;

    I2pSession(std::shared_ptr<Inner> inner) :
        _inner(std::move(inner))
    {}

private:
    // Shared with a keep-alive coroutine.
    std::shared_ptr<Inner> _inner;
};

} // namespace
