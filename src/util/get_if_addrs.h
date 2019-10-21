#pragma once

#include "../namespaces.h"
#include <set>
#include <boost/asio/ip/address.hpp>

namespace ouinet { namespace util {

std::set<asio::ip::address> get_if_addrs(sys::error_code&);

}} // namespaces
