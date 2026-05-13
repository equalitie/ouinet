#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "handshake.h"
#include "or_throw.h"
#include "util/async.h"
#include "generic_stream.h"
#include "namespaces.h"

namespace ouinet::i2p_direct {

static const std::string MAGIC = "i2p-ouinet";

sys::error_code perform_handshake(GenericStream& conn, Async yield) {
    auto wr = asio::async_write(conn, asio::buffer(MAGIC), yield);
    if (!wr.has_value()) return wr.error();
    
    std::string buffer(MAGIC.size(), 'x');

    auto rr = asio::async_read(conn, asio::buffer(buffer), yield);
    if (!rr.has_value()) return rr.error();
    
    if (buffer != MAGIC) {
        // TODO: We should return `std::errc::protocol_error`, but need to
        // figure out how to convert it to `boost::system::error_code`.
        return asio::error::no_protocol_option;
    }

    return {};
}

} // namespaces
