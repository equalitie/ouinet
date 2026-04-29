#pragma once

#include "address.h"
#include "session.h"

#include <boost/beast/http/status.hpp>
#include <set>

namespace ouinet {

class Async;
namespace bittorrent { class NodeID; }

class I2pTrackerClient {
public:
    struct Error {
        struct HttpSend {
            sys::error_code ec;
            friend std::ostream& operator<<(std::ostream& os, const HttpSend& e) {
                return os << "HttpSend{" << e.ec.message() << "}";
            }
        };
        struct HttpRecv {
            sys::error_code ec;
            friend std::ostream& operator<<(std::ostream& os, const HttpRecv& e) {
                return os << "HttpRecv{" << e.ec.message() << "}";
            }
        };
        struct InvalidResponse : std::string {
            friend std::ostream& operator<<(std::ostream& os, const InvalidResponse& e) {
                return os << "InvalidResponse{" << static_cast<std::string const&>(e) << "}";
            }
        };
        struct HttpResult {
            http::status status;
            friend std::ostream& operator<<(std::ostream& os, const HttpResult& e) {
                return os << "HttpResult{" << e.status << "}";
            }
        };
        struct SendRequest {
            std::variant<
                I2pSession::Error::Connect,
                HttpSend,
                HttpRecv,
                HttpResult> value;
            friend std::ostream& operator<<(std::ostream& os, const SendRequest& e) {
                return std::visit([&os] (auto& e) -> auto& {
                    return os << "SendRequest{" << e << "}"; }, e.value);
            }
        };
        struct Announce : SendRequest {
            friend std::ostream& operator<<(std::ostream& os, const Announce& e) {
                return os << "Announce{" << static_cast<const SendRequest&>(e) << "}";
            }
        };
        struct GetPeers {
            std::variant<SendRequest, InvalidResponse> value;
            friend std::ostream& operator<<(std::ostream& os, const GetPeers& e) {
                return std::visit([&os] (auto& e) -> auto&
                    { return os << "GetPeers{" << e << "}"; }, e.value);
            }
        };
    };

    I2pTrackerClient(std::shared_ptr<I2pSession> session, I2pAddress tracker_addr):
        _session(std::move(session)),
        _tracker_addr(std::move(tracker_addr))
    {}

    [[nodiscard]]
    std::expected<void, Error::Announce>
    announce(bittorrent::NodeID infohash, Async);

    [[nodiscard]]
    std::expected<
        std::set<I2pAddress>,
        Error::GetPeers
    >
    get_peers(bittorrent::NodeID infohash, Async);

    I2pTrackerClient(I2pTrackerClient const&) = delete;
    I2pTrackerClient(I2pTrackerClient &&) = delete;

    asio::any_io_executor get_executor() {
        return _session->get_executor();
    }

    std::shared_ptr<I2pSession> get_session() {
        return _session;
    }

private:
    [[nodiscard]]
    std::expected<std::string, Error::SendRequest>
    send_request(const std::string& target, Async);

private:
    std::shared_ptr<I2pSession> _session;
    I2pAddress _tracker_addr;
};

} // namespace
