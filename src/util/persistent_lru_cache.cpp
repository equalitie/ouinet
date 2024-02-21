#include "persistent_lru_cache.h"

#include <chrono>
#include <iostream>

#include "hash.h"

using namespace std;
using boost::string_view;

#ifdef _WIN32
#   if !BOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE
#       error "OS does not have a support for Windows random access handles"
#   endif
#else
#   if !BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
#       error "OS does not have a support for POSIX stream descriptors"
#   endif
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
    return dir / util::bytes::to_hex(util::sha1_digest(key));
}

bool is_cache_entry(const struct dirent* entry, boost::filesystem::path& dir) {
#ifdef _WIN32
    auto entry_path = dir / entry->d_name;
    bool is_regular = boost::filesystem::is_regular_file(entry_path);
#else
    bool is_regular = entry->d_type == DT_REG;
#endif
    return (is_regular
            && strstr(entry->d_name, temp_file_prefix) != entry->d_name);
}

}}} // namespaces
