#pragma once

#include <string>
#include <vector>

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/variant.hpp>

#include "util/signal.h"
#include "namespaces.h"

namespace ouinet {

class GenericStream;

namespace http_response {

struct Head : public http::response_header<>  {
    using Base = http::response_header<>;
    using Base::Base;
    Head(const Head&) = default;
    Head(Head&&) = default;
    Head& operator=(const Head&) = default;
    Head(const Base& b) : Base(b) {}
    Head(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Head& other) const;

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
};

struct Body : public std::vector<uint8_t> {
    using Base = std::vector<uint8_t>;

    Body(Base data) : Base(move(data)) {}

    Body(const Body&) = default;
    Body(Body&&) = default;
    Body& operator=(const Body&) = default;

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
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

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
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

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
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

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
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

    void async_write(GenericStream&, Cancel, asio::yield_context) const;
};

}} // namespace ouinet::http_response
