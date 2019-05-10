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

static const auto temp_file_prefix = "tmp.";
static const auto temp_file_model = std::string(temp_file_prefix) + "%%%%-%%%%-%%%%-%%%%";

uint64_t ms_since_epoch()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    return duration_cast<milliseconds>(now.time_since_epoch()).count();
}

fs::path path_from_key(const fs::path& dir, const std::string& key) {
    return dir / util::bytes::to_hex(util::sha1(key));
}

bool is_cache_entry(const struct dirent* entry) {
    return (entry->d_type == DT_REG
            && strstr(entry->d_name, temp_file_prefix) != entry->d_name);
}

fs::path temp_file_along(const fs::path& p, sys::error_code& ec) {
    return p.parent_path() / fs::unique_path(temp_file_model, ec);
}

}}} // namespaces
