#include "request_routing.h"

using namespace ouinet;

enum request_mechanism route_request( http::request<http::string_body> req
                                    , sys::error_code& ec)
{
    ec = sys::error_code();
    return request_mechanism::origin;
}
