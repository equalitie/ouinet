#include "session.h"
#include "session_id.h"
#include "address.h"
#include "util/async.h"
#include "async_sleep.h"

namespace ouinet {

using namespace std::chrono_literals;
using Error = I2pSession::Error;

struct I2pSession::Inner {
    Sam sam;
    SessionId id;
    I2pAddress local_addr;
    Cancel cancel;

    Inner(Sam sam, SessionId id, I2pAddress local_addr):
        sam(std::move(sam)), id(std::move(id)), local_addr(std::move(local_addr))
    {}
};

/* static */
std::expected<I2pSession, Error::Create> I2pSession::create(Async yield, std::optional<asio::ip::tcp::endpoint> sam_ep) {
    auto error = [] (auto&& e) { return std::unexpected(Error::Create { std::move(e) }); };

    auto ep = sam_ep ? *sam_ep : default_endpoint();

    auto sam = Sam::connect(ep, yield);
    if (!sam) return error(sam.error());

    auto slot1 = yield.cancel_slot([&] { sam->close(); });

    auto id = SessionId::random();

    auto c_rs = sam->create_session(id, yield);
    if (!c_rs) return error(c_rs.error());

    auto inner = std::make_shared<Inner>(std::move(*sam), std::move(id), std::move(*c_rs));

    // Keep-alive coroutine.
    yield.spawn([inner] (Async yield) mutable {
        auto slot = inner->cancel.connect([&] { yield.cancel(); });
        while (true) {
            async_sleep(5s, yield);
            auto r = inner->sam.ping("", yield);
            if (!r.has_value()) {
                break;
            }
        }
    });

    return I2pSession(inner);
}

std::expected<asio::ip::tcp::socket, Error::Connect> I2pSession::connect(const I2pAddress& remote_addr, Async yield) {
    auto error = [] (auto&& e) { return std::unexpected(Error::Connect { std::move(e) }); };
    auto slot = _inner->cancel.connect([&] { yield.cancel(); });

    auto sam = Sam::connect(_inner->sam.remote_endpoint(), yield);
    if (!sam) return error(sam.error());

    // TODO: Validate response is OK
    auto i_rs = sam->invoke("STREAM CONNECT ID=" + _inner->id.value + " DESTINATION=" + remote_addr.value + " SILENT=false", yield);
    if (!i_rs) return error(i_rs.error());

    return std::move(sam->release_socket());
}

std::expected<asio::ip::tcp::socket, Error::Accept> I2pSession::accept(Async yield) {
    auto error = [] (auto&& e) { return std::unexpected(Error::Accept { std::move(e) }); };
    auto slot = _inner->cancel.connect([&] { yield.cancel(); });

    asio::ip::tcp::socket socket(yield.get_executor());

    auto sam = Sam::connect(_inner->sam.remote_endpoint(), yield);
    if (!sam) return error(sam.error());

    // TODO: Validate response is OK
    auto i_rs = sam->invoke("STREAM ACCEPT ID=" + _inner->id.value + " SILENT=false", yield);
    if (!i_rs) return error(i_rs.error());

    auto l_rs = sam->recv_line(yield);
    if (!l_rs) return error(Sam::Error::Invoke { l_rs.error() });

    return std::move(sam->release_socket());
}

std::expected<std::optional<I2pAddress>, Sam::Error::Lookup> I2pSession::lookup(const std::string& name, Async yield) {
    auto slot = _inner->cancel.connect([&] { yield.cancel(); });
    return _inner->sam.lookup(name, yield);
}

const I2pAddress& I2pSession::local_addr() const { return _inner->local_addr; }

asio::any_io_executor I2pSession::get_executor() {
    return _inner->sam.get_executor();
}

bool I2pSession::is_open() const {
    return _inner && _inner->sam.is_open();
}

I2pSession::~I2pSession() {
    if (_inner) _inner->cancel();
}

} // namespace
