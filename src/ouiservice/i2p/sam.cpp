#include "sam.h"
#include "util/async.h"
#include "util/wait_condition.h"
#include "session_id.h"
#include "address.h"
#include "namespaces.h"

#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>

namespace ouinet {

using Error = Sam::Error;

static
std::optional<std::string_view> read_until(std::string_view& in, char delim) {
    while (in.starts_with(' ')) in.remove_prefix(1);
    if (in.empty()) return {};
    auto pos = in.find(delim);
    if (pos == in.npos) {
        auto out = in;
        in = in.substr(in.size());
        return out;
    }
    auto out = in.substr(0, pos);
    in = in.substr(pos + 1);
    return out;
}

static
std::optional<std::string_view> read_token(std::string_view& in) {
    return read_until(in, ' ');
}

static
std::optional<std::string_view> read_key(std::string_view& in) {
    return read_until(in, '=');
}

struct Sam::Inner {
    asio::ip::tcp::socket socket;
    WaitCondition mutex;

    Inner(asio::ip::tcp::socket socket):
        socket(std::move(socket)),
        mutex(this->socket.get_executor())
    {}
};

Sam::Sam(asio::ip::tcp::socket socket) :
    _inner(std::make_unique<Inner>(std::move(socket)))
{}

Sam::Sam(Sam&& other) :
    _inner(std::move(other._inner))
{}

std::expected<void, Error::IoSend> Sam::send_line(const std::string& line, Async yield) {
    auto r = asio::async_write(_inner->socket, asio::buffer(line + '\n'), yield);
    if (!r) {
        if (_inner->socket.is_open()) _inner->socket.close();
        return std::unexpected(Error::IoSend { r.error() });
    }
    return std::expected<void, Error::IoSend>();
}

std::expected<std::string, Error::IoRecv> Sam::recv_line(Async yield) {
    // NOTE: Reading byte-by-byte instead of using `asio::async_read_until` to avoid
    // reading past the '\n'.
    std::string line;
    while (true) {
        char c;
        auto r = asio::async_read(_inner->socket, asio::buffer(&c, 1), yield);
        if (!r) {
            if (_inner->socket.is_open()) _inner->socket.close();
            return std::unexpected(Error::IoRecv { r.error() });
        }
        if (c == '\n') return line;
        line.push_back(c);
    }
};

/* static */
std::expected<Sam, Error::Connect> Sam::connect(asio::ip::tcp::endpoint ep, Async yield) {
    auto error = [] (auto&& e) { return std::unexpected(Error::Connect { std::move(e) }); };

    asio::ip::tcp::socket socket(yield.get_executor());

    auto slot = yield.cancel_slot([&] { if (socket.is_open()) socket.close(); });

    auto ec = socket.async_connect(ep, yield);
    if (ec) return error(Error::IoConnect { ec });

    Sam sam(std::move(socket));

    auto slot1 = yield.cancel_slot([&] { sam.close(); });

    auto h_rs = sam.handshake(yield);
    if (!h_rs) return error(h_rs.error());

    return sam;
}

std::expected<std::string, Error::Invoke> Sam::invoke(const std::string& request, Async yield) {
    _inner->mutex.wait(yield);
    auto lock = _inner->mutex.lock();

    auto send_r = send_line(request, yield);
    if (!send_r) return std::unexpected(Error::Invoke { send_r.error() });
    auto recv_r = recv_line(yield);
    if (!recv_r) return std::unexpected(Error::Invoke { recv_r.error() });
    return std::move(*recv_r);
}

std::expected<I2pAddress, Error::CreateSession> Sam::create_session(SessionId const& session_id, Async yield) {
    auto error = [] (auto e) { return std::unexpected(Error::CreateSession { std::move(e) }); };

    auto keypair = dest_generate(yield);
    if (!keypair) return error(keypair.error());

    std::string request =
        "SESSION CREATE"
            " STYLE=STREAM"
            " ID=" + session_id.value +
            " DESTINATION=" + keypair->priv +
            " i2cp.leaseSetEncType=4,0";

    auto response = invoke(request, yield);
    if (!response) return error(response.error());

    auto proto_error = [&] () { return error(Error::UnexpectedResponse { std::move(request), std::move(*response) }); };

    std::string_view rs(*response);
    if (read_token(rs) != "SESSION" || read_token(rs) != "STATUS" || read_key(rs) != "RESULT") return proto_error();

    auto result = read_token(rs);
    if (!result) return proto_error();

    if (*result == "DUPLICATED_ID") {
        return error(Error::Result::DuplicatedId{});
    }
    else if (*result == "DUPLICATED_DEST") {
        return error(Error::Result::DuplicatedDest{});
    }
    else if (*result == "INVALID_KEY") {
        return error(Error::Result::InvalidKey{});
    }
    else if (*result == "I2P_ERROR") {
        if (read_key(rs) != "MESSAGE") return proto_error();
        auto message = read_token(rs);
        if (!message) return proto_error();
        return error(Error::Result::I2pError { std::string(*message) });
    }
    else if (*result != "OK") {
        return proto_error();
    }

    auto dst_key = read_key(rs);
    if (!dst_key) return proto_error();
    auto dst_val = read_token(rs);
    if (!dst_val) return proto_error();

    auto local_addr = I2pAddress::parse(keypair->pub);
    if (!local_addr) return error(Error::InvalidAddress { std::move(keypair->pub) });

    return std::move(*local_addr);
}

std::expected<Sam::Keypair, Error::DestGenerate> Sam::dest_generate(Async yield) {
    auto error = [] (auto e) { return std::unexpected(Error::DestGenerate { std::move(e) }); };
    std::string request = "DEST GENERATE SIGNATURE_TYPE=7";
    auto response = invoke(request, yield);
    if (!response.has_value()) return error(response.error());

    auto proto_error = [&] () { return error(Error::UnexpectedResponse { std::move(request), std::move(*response) }); };

    std::string_view rs(*response);

    if (read_token(rs) != "DEST" || read_token(rs) != "REPLY" || read_key(rs) != "PUB")
        return proto_error();

    auto pub = read_token(rs);
    if (!pub) return proto_error();

    if (read_key(rs) != "PRIV") return proto_error();

    auto priv = read_token(rs);
    if (!priv) return proto_error();

    return Keypair { std::string(*pub), std::string(*priv) };
}

std::expected<void, Error::Handshake> Sam::handshake(Async yield) {
    auto error = [] (auto e) { return std::unexpected(Error::Handshake { std::move(e) }); };

    auto request = "HELLO VERSION MIN=3.1 MAX=3.3";
    auto response = invoke(request, yield);
    if (!response) return error(response.error());

    auto proto_error = [&] () { return error(Error::UnexpectedResponse { std::move(request), std::move(*response) }); };

    std::string_view rs(*response);
    if (read_token(rs) != "HELLO" || read_token(rs) != "REPLY" || read_key(rs) != "RESULT") return proto_error();

    auto result = read_token(rs);
    if (!result) return proto_error();

    if (*result == "NOVERSION") {
        return error(Error::Result::NoVersion {});
    }
    else if (*result == "I2P_ERROR") {
        if (read_key(rs) != "MESSAGE") return proto_error();
        auto message = read_token(rs);
        if (!message) return proto_error();
        return error(Error::Result::I2pError { std::string(*message) });
    }
    else if (*result != "OK") {
        return proto_error();
    }
    // TODO: validate returned version
    return std::expected<void, Error::Handshake>();
}

std::expected<std::optional<I2pAddress>, Error::Lookup> Sam::lookup(const std::string& name, Async yield) {
    auto error = [] (auto e) { return std::unexpected(Error::Lookup { std::move(e) }); };

    auto request = "NAMING LOOKUP NAME=" + name;
    auto response = invoke(request, yield);
    if (!response) return error(response.error());

    auto proto_error = [&] () { return error(Error::UnexpectedResponse { std::move(request), std::move(*response) }); };

    std::string_view rs(*response);
    if (read_token(rs) != "NAMING" || read_token(rs) != "REPLY" || read_key(rs) != "RESULT") return proto_error();

    auto result = read_token(rs);
    if (!result) return proto_error();

    if (*result == "OK") {
        auto key_name = read_key(rs);
        if (!key_name.has_value()) return proto_error();
        if (*key_name != "NAME") return proto_error();
        auto value = read_token(rs);
        if (!value.has_value()) return proto_error();
        auto addr = I2pAddress::parse(*value);
        if (!addr) return proto_error();
        return std::move(*addr);
    }
    else if (*result == "KEY_NOT_FOUND") {
        return std::optional<I2pAddress>();
    }
    else if (*result == "INVALID_KEY") {
        return error(Error::Result::InvalidKey{});
    } else {
        return proto_error();
    }
}

std::expected<std::string, Error::Ping> Sam::ping(const std::string& msg, Async yield) {
    auto error = [] (auto e) { return std::unexpected(Error::Ping { std::move(e) }); };

    auto request = "PING " + msg;

    auto response = invoke(request, yield);
    if (!response) return error(response.error());

    std::string_view rs(*response);

    if (read_token(rs) != "PONG")
        return error(Error::UnexpectedResponse { std::move(request), std::move(*response) });

    while (rs.starts_with(' ')) rs.remove_prefix(1);
    while (rs.ends_with(' ')) rs.remove_suffix(1);

    return std::string(rs);
}

asio::ip::tcp::socket Sam::release_socket() {
    return std::move(_inner->socket);
}

asio::any_io_executor Sam::get_executor() {
    return _inner->socket.get_executor();
}

bool Sam::is_open() const {
    return _inner && _inner->socket.is_open();
}

void Sam::close() {
    if (_inner && _inner->socket.is_open()) {
        _inner->socket.close();
    }
}

asio::ip::tcp::endpoint Sam::remote_endpoint() const {
    return _inner->socket.remote_endpoint();
}

Sam::~Sam() {
    close();
}

} // namespace
