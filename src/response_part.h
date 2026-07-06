#pragma once

#include <cstdint>
#include <string>
#include <cstdint>
#include <vector>

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/asio/write.hpp>
#include <boost/variant.hpp>

#include "util/variant.h"
#include "util/watch_dog.h"
#include "namespaces.h"
#include "util/async.h"
#include "api.h"

namespace ouinet::http_response {

namespace detail {
    template<class S, class D>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(S& s, const D& data, Async yield)
    {
        if (yield.is_cancelled()) return std::unexpected(asio::error::operation_aborted);
        auto cancelled = yield.cancel_slot([&] { if (s.is_open()) s.close(); });
        auto r = asio::async_write(s, data, yield);
        if (!r) return std::unexpected(r.error());
        return {};
    }

    template<class P, class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(P* p, S& s, Duration d, Async yield)
    {
        auto y = yield;
        auto wd = watch_dog(y.get_executor(), d, [&] {
            if (s.is_open()) s.close();
            y.cancel();
        });

        try {
            auto r = p->async_write(s, y);
            if (!r) return std::unexpected(r.error());
            return {};
        }
        catch (Async::Cancelled const&) {
            if (yield.is_cancelled()) throw;
            return std::unexpected(asio::error::timed_out);
        }
    }
}

struct OUINET_COMMON_API Head : public http::response_header<> {
    using Base = http::response_header<>;
    using Base::Base;
    Head(const Head&) = default;
    Head(Head&&) = default;
    Head& operator=(const Head&) = default;
    Head(const Base& b) : Base(b) {}
    Head(Base&& b) : Base(std::move(b)) {}

    const Base& base() const { return *this; }

    bool chunked() const {
        return Base::get_chunked_impl();
    }

    void chunked(bool value) {
        Base::set_chunked_impl(value);
    }

    bool keep_alive() const {
        return Base::get_keep_alive_impl(Base::version());
    }

    void keep_alive(bool value) {
        Base::set_keep_alive_impl(Base::version(), value);
    }

    bool operator==(const Head& other) const;

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        Head::writer headw(*this, Base::version(), Base::result_int());
        return detail::async_write(s, headw.get(), yield);
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Duration d, Async yield) const
    { return detail::async_write(this, s, d, yield); }
};

struct Body : public std::vector<uint8_t> {
    using Base = std::vector<uint8_t>;

    Body(Base data) : Base(std::move(data)) {}

    Body(const Body&) = default;
    Body(Body&&) = default;
    Body& operator=(const Body&) = default;

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        return detail::async_write(s, asio::buffer(*this), yield);
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(S& s, Duration d, Async yield) const
    { return detail::async_write(this, s, d, yield); }
};

struct ChunkHdr {
    size_t size; // Size of chunk body
    std::string exts;

    ChunkHdr() : size(0) {}
    ChunkHdr(size_t size, std::string exts)
        : size(size)
        , exts(std::move(exts))
    {}

    bool is_last() const {
        return size == 0;
    }

    bool operator==(const ChunkHdr& other) const {
        return size == other.size && exts == other.exts;
    }

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        if (size > 0) {
            return detail::async_write(s, http::chunk_header{size, exts}, yield);
        }
        else {  // `http::chunk_last` carries a trailer itself, do not use
            // NOTE: asio::buffer("0") creates a buffer of size 2, so we need
            // to be explicit about the size to not include the trailing \0

            std::array<asio::const_buffer, 3> bufs = {
                asio::buffer("0", 1),
                asio::buffer(exts),
                asio::buffer("\r\n", 2) };

            assert(bufs[1].size() == exts.size());
            auto r = asio::async_write(s, bufs, yield);
            if (!r) return std::unexpected(r.error());
            return {};
        }
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Duration d, Async yield) const
    { return detail::async_write(this, s, d, yield); }
};

struct ChunkBody : public std::vector<uint8_t> {
    size_t remain;

    using Base = std::vector<uint8_t>;

    ChunkBody(Base data, size_t remain)
        : Base(std::move(data))
        , remain(remain) {}

    ChunkBody(const ChunkBody&) = default;
    ChunkBody(ChunkBody&&) = default;
    ChunkBody& operator=(const ChunkBody&) = default;

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        auto r = asio::async_write(s, asio::buffer(*this), yield);

        if (!r) return std::unexpected(r.error());

        if (remain == 0) {
            return detail::async_write(s, http::chunk_crlf{}, yield);
        }

        return {};
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Duration d, Async yield) const
    { return detail::async_write(this, s, d, yield); }
};

struct OUINET_COMMON_API Trailer : public http::fields {
    using Base = http::fields;
    using Base::Base;
    Trailer(const Trailer&) = default;
    Trailer(Trailer&&) = default;
    Trailer& operator=(const Trailer&) = default;
    Trailer(const Base& b) : Base(b) {}
    Trailer(Base&& b) : Base(std::move(b)) {}

    bool operator==(const Trailer& other) const;

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        Trailer::writer trailerw(*this);
        return detail::async_write(s, trailerw.get(), yield);
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(S& s, Duration d, Async yield) const
    { return detail::async_write(this, s, d, yield); }
};

namespace detail {
using PartVariant = boost::variant<Head, ChunkHdr, ChunkBody, Body, Trailer>;
}

struct Part : public detail::PartVariant
{
    enum class Type {
        HEAD, BODY, CHUNK_HDR, CHUNK_BODY, TRAILER
    };

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

    Type type() const {
        return util::apply(*this,
                [](const Head&)      { return Type::HEAD; },
                [](const Body&)      { return Type::BODY; },
                [](const ChunkHdr&)  { return Type::CHUNK_HDR; },
                [](const ChunkBody&) { return Type::CHUNK_BODY; },
                [](const Trailer&)   { return Type::TRAILER; });
    }

    template<class S>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Async yield) const
    {
        return util::apply(*this, [&](const auto& p) { return p.async_write(s, yield); });
    }

    template<class S, class Duration>
    [[nodiscard]]
    std::expected<void, sys::error_code> async_write(S& s, Duration d, Async yield) const
    {
        return util::apply(*this, [&](const auto& p) { return p.async_write(s, d, yield); });
    }
};

OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, ouinet::http_response::Part::Type);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, Part const&);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, Head const&);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, ChunkHdr const&);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, ChunkBody const&);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, Body const&);
OUINET_COMMON_API std::ostream& operator<<(std::ostream& os, Trailer const&);

} // namespace ouinet::http_response
