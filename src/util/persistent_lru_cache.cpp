#include "persistent_lru_cache.h"

#include <chrono>
#include <iostream>
#include "sha1.h"

using namespace std;
using boost::string_view;

#if !BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
#error "OS does not have a support for POSIX stream descriptors"
#endif

namespace ouinet {
namespace util {
namespace persisten_lru_cache_detail {

uint64_t ms_since_epoch()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    return duration_cast<milliseconds>(now.time_since_epoch()).count();
}

fs::path path_from_key(const fs::path& dir, const std::string& key) {
    return dir / util::bytes::to_hex(util::sha1(key));
}

}}} // namespaces
