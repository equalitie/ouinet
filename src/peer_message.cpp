#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include "peer_message.h"
#include "util/keep_alive.h"
#include "parse/number.h"
#include "constants.h"

namespace ouinet {

PeerRequest PeerRequest::async_read(GenericStream& con, YieldContext yield) {
    http::request<http::empty_body> req;
    beast::flat_buffer con_rbuf;

    sys::error_code ec;
    http::async_read(con, con_rbuf, req, yield.native()[ec]);

    if (ec) return or_throw<PeerRequest>(yield, ec);
    if (con_rbuf.size() > 0) con.put_back(con_rbuf.data(), ec);
    if (ec) return or_throw<PeerRequest>(yield, ec);

    http::verb method = req.method();

    if (method != http::verb::get &&
            method != http::verb::connect &&
            method != http::verb::head &&
            method != http::verb::propfind) {
        return or_throw<PeerRequest>(yield, make_error_code(PeerRequestError::invalid_method));
    }

    if (method == http::verb::connect) {
        return PeerConnectRequest();
    }

    bool keep_alive = util::get_keep_alive(req);

    auto protocol_version_sw = req[http_::protocol_version_hdr];
    auto protocol_version = parse::number<uint16_t>(protocol_version_sw);

    // TODO: Not being used at the moment, the old reasoning was that even if the
    // version doesn't match ours we'd still serve the peer some content.
    if (!protocol_version) {
        return or_throw<PeerRequest>(yield, make_error_code(PeerRequestError::invalid_protocol_version));
    }

    auto resource_id = cache::ResourceId::from_hex(req.target());

    if (!resource_id) {
        return or_throw<PeerRequest>(yield, make_error_code(PeerRequestError::invalid_target));
    }

    std::optional<util::HttpRequestByteRange> range;

    {
        auto ranges = util::HttpRequestByteRange::parse(req[http::field::range]);
        if (ranges && ranges->size() == 1) {
            if (ranges->size() == 1) {
                range = (*ranges)[0];
            } else {
                // XXX: We currently support max 1 rage in the request
                return or_throw<PeerRequest>(yield, make_error_code(PeerRequestError::invalid_range));
            }
        }
    }

    return PeerCacheRequest{
        //std::move(req),
        method,
        keep_alive,
        std::move(*resource_id),
        std::move(range)
    };
}

void async_write_blob_type(BlobType blob_type, GenericStream& con, asio::yield_context yield) {
    uint8_t is_cyphertext = blob_type == BlobType::cypher_text ? 1 : 0;
    asio::async_write(con, asio::buffer(&is_cyphertext, 1), yield);
}

BlobType async_read_blob_type(GenericStream& con, asio::yield_context yield) {
    uint8_t is_cyphertext = -1;
    sys::error_code ec;
    asio::async_read(con, asio::buffer(&is_cyphertext, 1), yield[ec]);

    if (ec) return or_throw<BlobType>(yield, ec);

    switch (is_cyphertext) {
        case 0: return BlobType::plain_text;
        case 1: return BlobType::cypher_text;
        default: return or_throw<BlobType>(yield, make_error_code(PeerRequestError::invalid_blob_type));
    }
}

void PeerCacheRequest::print(std::ostream& os) const {
    os << "PeerCacheRequest\n";
    os << "  method:      " << _method << "\n";
    os << "  keep_alive:  " << _keep_alive << "\n";
    os << "  resource_id: " << _resource_id << "\n";
    if (_range) {
        os << "  range:       " << *_range << "\n";
    }
}

class PeerRequestErrorCategory: public sys::error_category {
public:
    const char* name() const noexcept {
        return "peer request error";
    }

    std::string message( int ev ) const {
        char buffer[ 64 ];
        return this->message( ev, buffer, sizeof(buffer));
    }

    char const* message(int ev, char * buffer, std::size_t len) const noexcept {
        switch(static_cast<PeerRequestError>(ev))
        {
            case PeerRequestError::success: return "no error";
            case PeerRequestError::invalid_method: return "invalid method";
            case PeerRequestError::invalid_protocol_version: return "invalid protocol version";
            case PeerRequestError::invalid_target: return "invalid target (ResourceId)";
            case PeerRequestError::invalid_range: return "invalid range";
            case PeerRequestError::invalid_blob_type: return "invalid blob type";
        }

        std::snprintf(buffer, len, "Unknown error %d", ev );
        return buffer;
    }
};

sys::error_category const& peer_request_error_category() {
    static const PeerRequestErrorCategory instance;
    return instance;
}
} // namespace ouinet
