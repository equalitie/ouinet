#pragma once

#include <expected>
#include <boost/system/error_code.hpp>

namespace sys = boost::system;

namespace ouinet {
   namespace util { class Url; }
   class Async;
   std::expected<
       util::Url,
       sys::error_code
   >
   random_url_from_wikipedia(Async);
} // namespace ouinet