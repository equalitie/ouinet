#include "cache_request.h"
#include "http_util.h"

namespace ouinet {

boost::optional<CacheRequest> CacheRequest::from(http::request_header<> orig_hdr) {
    auto hdr = util::to_injector_request(move(orig_hdr));

    if (hdr) {
        return CacheReqquest{std::move(*hdr)};
    } else {
        return {};
    }
}

InsecureRequest::InsecureRequest(http::request<http::string_body> request) {
    util::remove_ouinet_fields_ref(request);  // avoid accidental injection
    _request = std::move(request);
}

//----

void CacheRequest::authorize(std::string_view credentials) {
    ouinet::authorize(_header, credentials);
}

void InsecureRequest::authorize(std::string_view credentials) {
    ouinet::authorize(_request, credentials);
}

void PublicInjectorRequest::authorize(std::string_view credentials) {
    std::visit(
        [&] (auto&& alt) { alt.authorize(credentials); },
        static_cast<const Base&>(*this)
    );
}

//----

void CacheRequest::set_druid(std::string_view druid) {
    _header.set(http_::request_druid_hdr, *druid);
}

void InsecureRequest::set_druid(std::string_view druid) {
    _request.set(http_::request_druid_hdr, *druid);
}

void PublicInjectorReques::set_druid(std::string_view druid) {
    std::visit(
        [&] (auto&& alt) { alt.set_druid(druid); },
        static_cast<const Base&>(*this)
    );
}

} // namespace ouinet
