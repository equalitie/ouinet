#pragma once

#include <string>
#include <vector>

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/variant.hpp>
#include <boost/container/flat_map.hpp>

#include "namespaces.h"


namespace ouinet { namespace http_response {

namespace detail {
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
} // detail namespace

struct Head : public http::response_header<>  {
    using Base = http::response_header<>;
    using Base::Base;
    Head(const Head&) = default;
    Head(Head&&) = default;
    Head& operator=(const Head&) = default;
    Head(const Base& b) : Base(b) {}
    Head(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Head& other) const {
        using namespace detail;
        return fields_to_map(*this) == fields_to_map(other);
    }
};

struct Body : public std::vector<uint8_t> {
    bool is_last;

    using Base = std::vector<uint8_t>;

    Body(bool is_last, Base data)
        : Base(move(data))
        , is_last(is_last)
    {}

    Body(const Body&) = default;
    Body(Body&&) = default;
    Body& operator=(const Body&) = default;
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
};

struct Trailer : public http::fields {
    using Base = http::fields;
    using Base::Base;
    Trailer(const Trailer&) = default;
    Trailer(Trailer&&) = default;
    Trailer& operator=(const Trailer&) = default;
    Trailer(const Base& b) : Base(b) {}
    Trailer(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Trailer& other) const {
        using namespace detail;
        return fields_to_map(*this) == fields_to_map(other);
    }
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
};

}} // namespace ouinet::http_response
