#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/format.hpp>
#include <boost/container/flat_map.hpp>

#include "namespaces.h"
#include "generic_stream.h"
#include "or_throw.h"
#include "response_part.h"
#include "util/yield.h"
#include "util/variant.h"

namespace ouinet { namespace http_response {

static
boost::container::flat_map<boost::string_view, boost::string_view>
fields_to_map(const http::fields& fields) {
    using boost::container::flat_map;
    using boost::string_view;
    using Map = flat_map<string_view, string_view>;
    Map ret;
    ret.reserve(std::distance(fields.begin(), fields.end()));
    for (auto& f : fields) {
        ret.insert(Map::value_type(f.name_string(), f.value()));
    }
    return ret;
}

using asio::yield_context;

//--------------------------------------------------------------------
bool Head::operator==(const Head& other) const {
    if (version() != other.version()) return false;
    if (result_int() != other.result_int()) return false;
    return fields_to_map(*this) == fields_to_map(other);
}

bool Trailer::operator==(const Trailer& other) const {
    return fields_to_map(*this) == fields_to_map(other);
}

//--------------------------------------------------------------------
static
void async_write(const Head& head, GenericStream& con, yield_context yield)
{
    Head::writer headw(head, head.version(), head.result_int());
    asio::async_write(con, headw.get(), yield);
}

static
void async_write(const Body& body, GenericStream& con, yield_context yield)
{
    asio::async_write(con, asio::buffer(body), yield);
}

static
void async_write(const ChunkHdr& ch_hdr, GenericStream& con, yield_context yield)
{
    if (ch_hdr.size > 0)
        asio::async_write(con, http::chunk_header{ch_hdr.size, ch_hdr.exts}, yield);
    else {  // `http::chunk_last` carries a trailer itself, do not use
        static const auto hdrf = "0%s\r\n";
        auto hdr = (boost::format(hdrf) % ch_hdr.exts).str();
        asio::async_write(con, asio::buffer(hdr), yield);
    }
}

static
void async_write(const ChunkBody& ch_b, GenericStream& con, yield_context yield)
{
    sys::error_code ec;
    asio::async_write(con, asio::buffer(ch_b), yield[ec]);

    if (ec) return or_throw(yield, ec);

    if (ch_b.remain == 0)
        asio::async_write(con, http::chunk_crlf{}, yield[ec]);
}

static
void async_write(const Trailer& trailer, GenericStream& con, yield_context yield)
{
    Trailer::writer trailerw(trailer);
    asio::async_write(con, trailerw.get(), yield);
}

//--------------------------------------------------------------------
template<class T>
static
void async_write_c(const T& t, GenericStream& con, Cancel& cancel, yield_context yield)
{
    auto cancelled = cancel.connect([&] { con.close(); });
    sys::error_code ec;
    async_write(t, con, yield[ec]);
    if (cancelled) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

//--------------------------------------------------------------------
void
Head::async_write( GenericStream& con
                 , Cancel cancel
                 , asio::yield_context yield) const
{
    async_write_c(*this, con, cancel, yield);
}

void
Body::async_write( GenericStream& con
                 , Cancel cancel
                 , asio::yield_context yield) const
{
    async_write_c(*this, con, cancel, yield);
}

void
ChunkHdr::async_write( GenericStream& con
                     , Cancel cancel
                     , asio::yield_context yield) const
{
    async_write_c(*this, con, cancel, yield);
}

void
ChunkBody::async_write( GenericStream& con
                      , Cancel cancel
                      , asio::yield_context yield) const
{
    async_write_c(*this, con, cancel, yield);
}

void
Trailer::async_write( GenericStream& con
                    , Cancel cancel
                    , asio::yield_context yield) const
{
    async_write_c(*this, con, cancel, yield);
}

//--------------------------------------------------------------------
void
Part::async_write( GenericStream& con
                 , Cancel cancel
                 , asio::yield_context yield) const
{
    util::apply(*this, [&] (const auto& p) {
        p.async_write(con, cancel, yield);
    });
}

}} // namespace ouinet::http_response
