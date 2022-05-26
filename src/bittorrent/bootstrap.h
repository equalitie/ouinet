#pragma once

#include <string>

#include <boost/asio/ip/udp.hpp>
#include <boost/variant.hpp>

#include "../namespaces.h"

namespace ouinet {
namespace bittorrent {
namespace bootstrap {

static const unsigned short default_port = 6881;

using Address = boost::variant< asio::ip::udp::endpoint
                              , std::string /* domain_name[:port] */>;

} // bootstrap namespace
} // bittorrent namespace
} // ouinet namespace
