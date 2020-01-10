#pragma once

#include <string>
#include <vector>

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/asio/write.hpp>
#include <boost/variant.hpp>
#include <boost/format.hpp>

#include "util/signal.h"
#include "util/variant.h"
#include "namespaces.h"
#include "or_throw.h"

namespace ouinet {

namespace http_response {

namespace detail {
    template<class P, class S>
    void async_write_c(const P* p, S& s, Cancel& c, asio::yield_context y)
    {
        auto cancelled = c.connect([&] { s.close(); });
        sys::error_code ec;
        p->async_write(s, y[ec]);
        if (cancelled) ec = asio::error::operation_aborted;
        return or_throw(y, ec);
    }
}

struct Head : public http::response_header<> {
    using Base = http::response_header<>;
    using Base::Base;
    Head(const Head&) = default;
    Head(Head&&) = default;
    Head& operator=(const Head&) = default;
    Head(const Base& b) : Base(b) {}
    Head(Base&& b) : Base(std::move(b)) {}

    bool chunked() const {
        return Base::get_chunked_impl();
    }

    bool keep_alive() const {
        return Base::get_keep_alive_impl(Base::version());
    }

    bool operator==(const Head& other) const;

    template<class S>
    void async_write(S& s, asio::yield_context yield) const
    {
        Head::writer headw(*this, Base::version(), Base::result_int());
        asio::async_write(s, headw.get(), yield);
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

struct Body : public std::vector<uint8_t> {
    using Base = std::vector<uint8_t>;

    Body(Base data) : Base(move(data)) {}

    Body(const Body&) = default;
    Body(Body&&) = default;
    Body& operator=(const Body&) = default;

    template<class S>
    void async_write(S& s, asio::yield_context yield) const
    {
        asio::async_write(s, asio::buffer(*this), yield);
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

struct ChunkHdr {
    size_t size; // Size of chunk body
    std::string exts;

    ChunkHdr() : size(0) {}
    ChunkHdr(size_t size, std::string exts)
        : size(size)
        , exts(std::move(exts))
    {}

    bool operator==(const ChunkHdr& other) const {
        return size == other.size && exts == other.exts;
    }

    template<class S>
    void async_write(S& s, asio::yield_context yield) const
    {
        if (size > 0) {
            asio::async_write(s, http::chunk_header{size, exts}, yield);
        }
        else {  // `http::chunk_last` carries a trailer itself, do not use
            static const auto hdrf = "0%s\r\n";
            auto hdr = (boost::format(hdrf) % exts).str();
            asio::async_write(s, asio::buffer(hdr), yield);
        }
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

struct ChunkBody : public std::vector<uint8_t> {
    size_t remain;

    using Base = std::vector<uint8_t>;

    ChunkBody(const Base& data, size_t remain)
        : Base(data)
        , remain(remain) {}

    ChunkBody(const ChunkBody&) = default;
    ChunkBody(ChunkBody&&) = default;
    ChunkBody& operator=(const ChunkBody&) = default;

    template<class S>
    void async_write(S& s, asio::yield_context yield) const
    {
        sys::error_code ec;
        asio::async_write(s, asio::buffer(*this), yield[ec]);
    
        if (ec) return or_throw(yield, ec);
    
        if (remain == 0) {
            asio::async_write(s, http::chunk_crlf{}, yield[ec]);
        }
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

struct Trailer : public http::fields {
    using Base = http::fields;
    using Base::Base;
    Trailer(const Trailer&) = default;
    Trailer(Trailer&&) = default;
    Trailer& operator=(const Trailer&) = default;
    Trailer(const Base& b) : Base(b) {}
    Trailer(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Trailer& other) const;

    template<class S>
    void async_write(S& s, asio::yield_context yield) const
    {
        Trailer::writer trailerw(*this);
        asio::async_write(s, trailerw.get(), yield);
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

namespace detail {
using PartVariant = boost::variant<Head, ChunkHdr, ChunkBody, Body, Trailer>;
}

struct Part : public detail::PartVariant
{
    using Base = detail::PartVariant;
    using Base::Base;
    Part() = default;
    Part(const Part&) = default;
    Part(Part&&) = default;
    Part& operator=(const Part&) = default;
    Part(const Base& b) : Base(b) {}
    Part(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Part& that) const {
        return static_cast<const Base&>(*this)
            == static_cast<const Base&>(that);
    }

    Head*      as_head()       { return boost::get<Head>     (this); }
    Body*      as_body()       { return boost::get<Body>     (this); }
    ChunkHdr*  as_chunk_hdr()  { return boost::get<ChunkHdr> (this); }
    ChunkBody* as_chunk_body() { return boost::get<ChunkBody>(this); }
    Trailer*   as_trailer()    { return boost::get<Trailer>  (this); }

    const Head*      as_head()       const { return boost::get<Head>     (this); }
    const Body*      as_body()       const { return boost::get<Body>     (this); }
    const ChunkHdr*  as_chunk_hdr()  const { return boost::get<ChunkHdr> (this); }
    const ChunkBody* as_chunk_body() const { return boost::get<ChunkBody>(this); }
    const Trailer*   as_trailer()    const { return boost::get<Trailer>  (this); }

    bool is_head()       const { return as_head()       != nullptr; }
    bool is_body()       const { return as_body()       != nullptr; }
    bool is_chunk_hdr()  const { return as_chunk_hdr()  != nullptr; }
    bool is_chunk_body() const { return as_chunk_body() != nullptr; }
    bool is_trailer()    const { return as_trailer()    != nullptr; }

    template<class S>
    void async_write(S& s, asio::yield_context y) const
    {
        util::apply(*this, [&](const auto& p) { p.async_write(s, y); });
    }

    template<class S>
    void async_write(S& s, Cancel& c, asio::yield_context y) const
    { return detail::async_write_c(this, s, c, y); }
};

}} // namespace ouinet::http_response
