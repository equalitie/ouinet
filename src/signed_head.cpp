#include "signed_head.h"
#include "parse/number.h"
#include "http_util.h"

using namespace std;
using namespace ouinet;

/* static */
SignedHead SignedHead::parse(http_response::Head raw_head, sys::error_code& ec) {
    SignedHead h(std::move(raw_head));

    auto tsh = util::http_injection_ts(h);
    auto ts = parse::number<time_t>(tsh);

    if (!ts) {
        ec = sys::errc::make_error_code(sys::errc::bad_message);
        return h;
    }

    h._time_stamp = boost::posix_time::from_time_t(*ts);

    return h;
}


