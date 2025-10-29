#include "request.h"
#include "http_util.h"
#include "util/keep_alive.h"
#include "authenticate.h"

namespace ouinet {

boost::optional<CacheRequest> CacheRequest::from(http::request_header<> orig_hdr) {
    // TODO: Keep alive should be handled by `to_injector_request`
    bool keepalive = util::get_keep_alive(orig_hdr);
    auto hdr = util::to_injector_request(move(orig_hdr));

    if (hdr) {
        util::set_keep_alive(*hdr, keepalive);
        return CacheRequest(std::move(*hdr));
    } else {
        return {};
    }
}

InsecureRequest::InsecureRequest(http::request<http::string_body> request) {
    util::remove_ouinet_fields_ref(request);  // avoid accidental injection
    _request = std::move(request);
}

void CacheRequest::set_if_none_match(std::string_view if_none_match) {
    _header.set(http::field::if_none_match, if_none_match);
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
        static_cast<Base&>(*this)
    );
}

//----

void CacheRequest::set_druid(std::string_view druid) {
    _header.set(http_::request_druid_hdr, druid);
}

void InsecureRequest::set_druid(std::string_view druid) {
    _request.set(http_::request_druid_hdr, druid);
}

void PublicInjectorRequest::set_druid(std::string_view druid) {
    std::visit(
        [&] (auto&& alt) { alt.set_druid(druid); },
        static_cast<Base&>(*this)
    );
}

const http::request_header<>& PublicInjectorRequest::header() const {
    return std::visit(
        [] (const auto& alt) -> const http::request_header<>&
        { return alt.header(); },
        static_cast<const Base&>(*this)
    );
}

bool PublicInjectorRequest::can_inject() const {
    return std::visit(
        [] (const auto& alt) { return alt.can_inject(); },
        static_cast<const Base&>(*this)
    );
}

} // namespace ouinet
