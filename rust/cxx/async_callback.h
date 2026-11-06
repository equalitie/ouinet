#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/spawn.hpp>
#include <string_view>

namespace ouinet::metrics {

using AsyncCallback = std::function<void(std::string_view /* record name */
                                         ,
                                         boost::asio::const_buffer
                                         /* record content */
                                         ,
                                         boost::asio::yield_context)>;

}
