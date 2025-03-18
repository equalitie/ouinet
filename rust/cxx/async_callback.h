#pragma once

#include <boost/asio/spawn.hpp>
#include <string_view>

namespace ouinet::metrics {

using AsyncCallback = std::function<
    void( std::string_view /* record name */
        , std::string_view /* record content */
        , asio::yield_context
        )>;

}
