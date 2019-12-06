//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

// This is a variation of Boost Beast's last chunk implementation
// to allow it to support chunk extensions as per RFC 7230 section 4.1.
// See <https://github.com/boostorg/beast/issues/1644> for more info.

#ifndef BOOST_BEAST_HTTP_CHUNK_LAST_X_HPP
#define BOOST_BEAST_HTTP_CHUNK_LAST_X_HPP

#include <boost/beast/http/chunk_encode.hpp>

namespace boost {
namespace beast {
namespace http {

/** A chunked-encoding last chunk
*/
template<class Trailer = chunk_crlf>
class chunk_last_x
{
    static_assert(
        is_fields<Trailer>::value ||
        boost::asio::is_const_buffer_sequence<Trailer>::value,
        "Trailer requirements not met");

    using buffers_type = typename
        detail::buffers_or_fields<Trailer>::type;

    using view_type =
        buffers_cat_view<
            detail::chunk_size,     // "0" for the last chunk
            boost::asio::const_buffer,   // chunk-extenstions
            chunk_crlf,             // CRLF
            buffers_type>;          // Trailer (includes CRLF)

    template<class Allocator>
    buffers_type
    prepare(Trailer const& trailer, Allocator const& alloc);

    buffers_type
    prepare(Trailer const& trailer, std::true_type);

    buffers_type
    prepare(Trailer const& trailer, std::false_type);

    std::shared_ptr<void> trsp_;
    std::shared_ptr<
        detail::chunk_extensions> exts_;
    view_type view_;

public:
    /** Constructor

        The last chunk will have no chunk extensions
        and an empty trailer.

        @see https://tools.ietf.org/html/rfc7230#section-4.1
    */
    chunk_last_x();

    /** Constructor

        The last chunk will have the passed chunk extensions
        and an empty trailer.

        @param extensions The chunk extensions string. This
        string must be formatted correctly as per rfc7230,
        using this BNF syntax:
        @code
            chunk-ext       = *( ";" chunk-ext-name [ "=" chunk-ext-val ] )
            chunk-ext-name  = token
            chunk-ext-val   = token / quoted-string
        @endcode
        The data pointed to by this string view must remain
        valid for the lifetime of any operations performed on
        the object.

        @see https://tools.ietf.org/html/rfc7230#section-4.1
    */
    explicit
    chunk_last_x(string_view extensions);

    /** Constructor

        The last chunk will have the passed trailer
        and no chunk extensions.
        The default allocator is used to provide storage for the
        trailer.

        @param trailer The trailer to use. This may be
        a type meeting the requirements of either Fields
        or ConstBufferSequence. If it is a ConstBufferSequence,
        the trailer must be formatted correctly as per rfc7230
        including a CRLF on its own line to denote the end
        of the trailer.

        @see https://tools.ietf.org/html/rfc7230#section-4.1
    */
    explicit
    chunk_last_x(Trailer const& trailer);

    /** Constructor

        The last chunk will have the passed chunk extensions
        and trailer.
        The default allocator is used to provide storage for the
        trailer.

        @param extensions The chunk extensions string. This
        string must be formatted correctly as per rfc7230,
        using this BNF syntax:
        @code
            chunk-ext       = *( ";" chunk-ext-name [ "=" chunk-ext-val ] )
            chunk-ext-name  = token
            chunk-ext-val   = token / quoted-string
        @endcode
        The data pointed to by this string view must remain
        valid for the lifetime of any operations performed on
        the object.

        @param trailer The trailer to use. This may be
        a type meeting the requirements of either Fields
        or ConstBufferSequence. If it is a ConstBufferSequence,
        the trailer must be formatted correctly as per rfc7230
        including a CRLF on its own line to denote the end
        of the trailer.

        @see https://tools.ietf.org/html/rfc7230#section-4.1
    */
    explicit
    chunk_last_x(string_view extensions, Trailer const& trailer);

    /** Constructor

        The last chunk will have no chunk extensions
        and the passed trailer.

        @param trailer The trailer to use. This type must
        meet the requirements of Fields.

        @param allocator The allocator to use for storing temporary
        data associated with the serialized trailer buffers.

        @see https://tools.ietf.org/html/rfc7230#section-4.1
    */
#if BOOST_BEAST_DOXYGEN
    template<class Allocator>
    chunk_last_x(
        Trailer const& trailer,
        Allocator const& allocator);
#else
    template<
        class DeducedTrailer,
        class Allocator,
        class = typename std::enable_if<
            is_fields<DeducedTrailer>::value>::type>
    chunk_last_x(
        DeducedTrailer const& trailer,
        Allocator const& allocator);
#endif

    //-----

    /// Required for <em>ConstBufferSequence</em>
    chunk_last_x(chunk_last_x const&) = default;

    /// Required for <em>ConstBufferSequence</em>
#if BOOST_BEAST_DOXYGEN
    using value_type = __implementation_defined__;
#else
    using value_type =
        typename view_type::value_type;
#endif

    /// Required for <em>ConstBufferSequence</em>
#if BOOST_BEAST_DOXYGEN
    using const_iterator = __implementation_defined__;
#else
    using const_iterator =
        typename view_type::const_iterator;
#endif

    /// Required for <em>ConstBufferSequence</em>
    const_iterator
    begin() const
    {
        return view_.begin();
    }

    /// Required for <em>ConstBufferSequence</em>
    const_iterator
    end() const
    {
        return view_.end();
    }
};

//------------------------------------------------------------------------------

/** Returns a @ref chunk_last_x

    @note This function is provided as a notational convenience
    to omit specification of the class template arguments.
*/
inline
chunk_last_x<chunk_crlf>
make_chunk_last_x()
{
    return chunk_last_x<chunk_crlf>{};
}

/** Returns a @ref chunk_last_x

    This function construct and returns a complete
    @ref chunk_last_x for a last chunk containing the
    specified chunk extensions and an empty trailer.

    @note This function is provided as a notational convenience
    to omit specification of the class template arguments.

    @param extensions The chunk extensions string.

    @param args Optional arguments passed to the @ref chunk_last_x
    constructor.
*/
template<class... Args>
chunk_last_x<chunk_crlf>
make_chunk_last_x(
    string_view extensions,
    Args&&... args)
{
    return chunk_last_x<chunk_crlf>{
        extensions, std::forward<Args>(args)...};
}

/** Returns a @ref chunk_last_x

    This function construct and returns a complete
    @ref chunk_last_x for a last chunk containing the
    specified trailers and no chunk extensions.

    @param trailer A ConstBufferSequence or 
    @note This function is provided as a notational convenience
    to omit specification of the class template arguments.

    @param args Optional arguments passed to the @ref chunk_last_x
    constructor.
*/
template<class Trailer, class... Args>
chunk_last_x<Trailer>
make_chunk_last_x(
    Trailer const& trailer,
    Args&&... args)
{
    return chunk_last_x<Trailer>{
        trailer, std::forward<Args>(args)...};
}

//------------------------------------------------------------------------------

template<class Trailer>
template<class Allocator>
auto
chunk_last_x<Trailer>::
prepare(Trailer const& trailer, Allocator const& allocator) ->
    buffers_type
{
    auto trsp = std::allocate_shared<typename
        Trailer::writer>(allocator, trailer);
    trsp_ = trsp;
    return trsp->get();
}

template<class Trailer>
auto
chunk_last_x<Trailer>::
prepare(Trailer const& trailer, std::true_type) ->
    buffers_type
{
    auto trsp = std::make_shared<
        typename Trailer::writer>(trailer);
    trsp_ = trsp;
    return trsp->get();
}

template<class Trailer>
auto
chunk_last_x<Trailer>::
prepare(Trailer const& trailer, std::false_type) ->
    buffers_type
{
    return trailer;
}

template<class Trailer>
chunk_last_x<Trailer>::
chunk_last_x()
    : view_(
        0,
        boost::asio::const_buffer{nullptr, 0},
        chunk_crlf{},
        Trailer{})
{
}

template<class Trailer>
chunk_last_x<Trailer>::
chunk_last_x(string_view extensions)
    : view_(
        0,
        boost::asio::const_buffer{
            extensions.data(), extensions.size()},
        chunk_crlf{},
        Trailer{})
{
}

template<class Trailer>
chunk_last_x<Trailer>::
chunk_last_x(Trailer const& trailer)
    : view_(
        0,
        boost::asio::const_buffer{nullptr, 0},
        chunk_crlf{},
        prepare(trailer, is_fields<Trailer>{}))
{
}

template<class Trailer>
chunk_last_x<Trailer>::
chunk_last_x(string_view extensions, Trailer const& trailer)
    : view_(
        0,
        boost::asio::const_buffer{
            extensions.data(), extensions.size()},
        chunk_crlf{},
        prepare(trailer, is_fields<Trailer>{}))
{
}

template<class Trailer>
template<class DeducedTrailer, class Allocator, class>
chunk_last_x<Trailer>::
chunk_last_x(
    DeducedTrailer const& trailer, Allocator const& allocator)
    : view_(
        0,
        boost::asio::const_buffer{nullptr, 0},
        chunk_crlf{},
        prepare(trailer, allocator))
{
}

} // http
} // beast
} // boost

#endif
