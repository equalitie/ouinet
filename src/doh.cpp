#include "doh.h"

namespace ouinet { namespace doh {

Request build_request( const std::string& name
                     , const std::string& base)
{
    // TODO: implement
    return Request{};
}

TcpLookup parse_response( const Response& rs
                        , const std::string& port
                        , sys::error_code& ec)
{
    // TODO: implement
    ec = asio::error::operation_not_supported;
    return TcpLookup{};
}

}} // ouinet::doh namespace
