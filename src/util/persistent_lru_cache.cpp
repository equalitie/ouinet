#include "persistent_lru_cache.h"

#include <chrono>
#include <iostream>

using namespace std;
using boost::string_view;

#if !BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
#error "OS does not have a support for POSIX stream descriptors"
#endif

namespace ouinet {
namespace util {
namespace persisten_lru_cache_detail {

// https://www.boost.org/doc/libs/1_69_0/libs/system/doc/html/system.html#ref_boostsystemerror_code_hpp
namespace errc = boost::system::errc;

void create_or_check_directory(const fs::path& dir, sys::error_code& ec)
{
    if (fs::exists(dir)) {
        if (!is_directory(dir)) {
            ec = make_error_code(errc::not_a_directory);
            return;
        }

        // TODO: Check if we can read/write
    } else {
        if (!create_directories(dir, ec)) {
            if (!ec) ec = make_error_code(errc::operation_not_permitted);
            return;
        }
        assert(is_directory(dir));
    }
}

uint64_t ms_since_epoch()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    return duration_cast<milliseconds>(now.time_since_epoch()).count();
}

}}} // namespaces
