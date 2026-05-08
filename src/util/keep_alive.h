#pragma once

#include <boost/beast/http/message.hpp>
#include <boost/beast/core/string_type.hpp>
#include <boost/beast/http/rfc7230.hpp>
#include <boost/beast/http/field.hpp>
#include "namespaces.h"

namespace ouinet::util {

// Boost.Beast doesn't export the `keep_alive` function from it's `header`
// class. Only from the `message`, which forces us to unnecessarily pass whole
// messages to functions that only require the header. This is a copy/paste
// implementation of the `keep_alive` function from Beast.
//
// https://github.com/boostorg/beast/issues/3041
template<class Fields>
bool
get_keep_alive(http::request_header<Fields> const& rq)
{
    auto const it = rq.find(http::field::connection);
    if(rq.version() < 11)
    {
        if(it == rq.end())
            return false;
        return http::token_list{
            it->value()}.exists("keep-alive");
    }
    if(it == rq.end())
        return true;
    return ! http::token_list{
        it->value()}.exists("close");
}

template<class Fields>
void
set_keep_alive(http::request_header<Fields>& rq, bool keep_alive)
{
    auto const value = rq[http::field::connection];
    beast::detail::temporary_buffer buf;

    boost::beast::http::detail::keep_alive_impl(buf, value, rq.version(), keep_alive);

    if(buf.empty())
        rq.erase(boost::beast::http::field::connection);
    else
        rq.set(boost::beast::http::field::connection, buf.view());
}

} // namespace ouinet::util
