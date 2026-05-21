#pragma once

#include "sam.h"
#include "../../util/cancel.h"
#include "declspec.h"

#include <variant>
#include <ostream>

namespace ouinet {

class Async;

class OUINET_DECL I2pSession {
public:
    struct Error {
        struct Create {
            using Value = std::variant<Sam::Error::Connect, Sam::Error::CreateSession>;
            Value value;
            friend std::ostream& operator<<(std::ostream& os, const Create& e) {
                return std::visit([&os] (auto& e) -> std::ostream&
                    { return os << "Create {" << e << "}"; },
                    e.value);
            }
            sys::error_code code() const {
                return std::visit([] (auto& e) { return e.code(); }, value);
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
            sys::error_code code() const {
                return std::visit([] (auto& e) { return e.code(); }, value);
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
            sys::error_code code() const {
                return std::visit([] (auto& e) { return e.code(); }, value);
            }
        };
    };

    I2pSession(I2pSession&& other) = default;

    [[nodiscard]]
    static std::expected<I2pSession, Error::Create> create(Async, std::optional<asio::ip::tcp::endpoint> sam_ep = {});

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
