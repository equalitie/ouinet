#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "handshake.h"
#include "../../or_throw.h"
#include "../../generic_stream.h"
#include "../../namespaces.h"

namespace ouinet::ouiservice::i2poui {

static const std::string MAGIC = "i2p-ouinet";

void perform_handshake(GenericStream& conn, Cancel& cancel, asio::yield_context yield) {
    sys::error_code ec;
    
    asio::async_write(conn, asio::buffer(MAGIC), yield[ec]);
    ec = compute_error_code(ec, cancel);
    if (ec) return or_throw(yield, ec);
    
    std::string buffer(MAGIC.size(), 'x');

    asio::async_read(conn, asio::buffer(buffer), yield[ec]);
    ec = compute_error_code(ec, cancel);
    if (ec) return or_throw(yield, ec);
    
    if (buffer != MAGIC) {
        // TODO: We should return `std::errc::protocol_error`, but need to
        // figure out how to convert it to `boost::system::error_code`.
        return or_throw(yield, asio::error::no_protocol_option);
    }
}

} // namespaces
