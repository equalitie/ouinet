#include "request.h"
#include "http_util.h"
#include "authenticate.h"
#include "cache/cache_entry.h"

namespace ouinet {

static boost::optional<std::string> extract_dht_group(http::request_header<>& hdr) {
    boost::optional<std::string> dht_group;

#if defined(__MACH__)
    std::string from_url(const std::string& url) {
        auto dhtgroup = std::move(url);

        boost::regex scheme("^[a-z][-+.0-9a-z]*://");
        dhtgroup = boost::regex_replace(dhtgroup, scheme, "");
        boost::regex trailing_slashes("/+$");
        dhtgroup = boost::regex_replace(dhtgroup, trailing_slashes, "");
        boost::regex leading_www("^www.");
        dhtgroup = boost::regex_replace(dhtgroup, leading_www, "");

        return dhtgroup;
    }

    // On iOS, it is not possible to inject headers into every request
    // Set the DHT group based on the referrer field or hostname (if referrer is not present)
    auto i = hdr.find(http::field::referer);
    if (i != hdr.end()) {
        dht_group = from_url(std::string(i->value()));
        hdr.erase(i);
    } else {
        dht_group = from_url(std::string(hdr.target()));
    }
#else
    auto i = hdr.find(http_::request_group_hdr);
    if (i != hdr.end()) {
        dht_group = std::string(i->value());
        hdr.erase(i);
    }
#endif

    return dht_group;
}

static bool is_private(http::request_header<> const& hdr) {
    bool ret = false;
    auto i = hdr.find(http_::request_private_hdr);
    if (i != hdr.end()) {
        ret = boost::iequals(i->value(), http_::request_private_true);
    }
    return ret;
}

boost::optional<CacheRequest> CacheRequest::from(http::request_header<> orig_hdr) {
    auto dht_group = extract_dht_group(orig_hdr);
    if (!dht_group) return {};

    if (orig_hdr.method() != http::verb::get && orig_hdr.method() != http::verb::head) {
        return {};
    }

    if (is_private(orig_hdr)) {
        LOG_WARN("Mutually exclusive header fields in request: ", http_::request_private_hdr, " and ", http_::request_group_hdr);
        return {};
    }

    // Check the original request did not have a body
    auto content_len_i = orig_hdr.find(http::field::content_length);

    if (content_len_i != orig_hdr.end()) {
        if (content_len_i->value() != "0") {
            return {};
        }
    }

    auto hdr = util::to_injector_request(move(orig_hdr));

    if (!hdr) {
        return {};
    }

    auto resource_id = cache::ResourceId::from_url(hdr->target());

    if (!resource_id) {
        return {};
    }

    return CacheRequest(std::move(*hdr), std::move(*resource_id), std::move(*dht_group));
}

//----

void CacheInjectRequest::authorize(std::string_view credentials) {
    ouinet::authorize(_header, credentials);
}

void CacheInjectRequest::set_druid(std::string_view druid) {
    _header.set(http_::request_druid_hdr, druid);
}

//----

CacheRetrieveRequest CacheRequest::to_retrieve_request() const {
    return CacheRetrieveRequest(_header.method(), _resource_id, _dht_group);
}

http::verb CacheRetrieveRequest::method() const {
    return _method;
}

CacheInjectRequest CacheRequest::to_inject_request() const {
    return CacheInjectRequest(_header, _resource_id, _dht_group);
}

void CacheRequest::set_if_none_match(std::string_view if_none_match) {
    _header.set(http::field::if_none_match, if_none_match);
}

//----

void InsecureRequest::authorize(std::string_view credentials) {
    ouinet::authorize(_request, credentials);
}

boost::optional<InsecureRequest> InsecureRequest::from(http::request<http::string_body> request) {
    if (is_private(request)) return {};
    util::remove_ouinet_fields_ref(request);  // avoid accidental injection
    return InsecureRequest(std::move(request));
}

void PublicInjectorRequest::authorize(std::string_view credentials) {
    std::visit(
        [&] (auto&& alt) { alt.authorize(credentials); },
        static_cast<Base&>(*this)
    );
}

void InsecureRequest::set_druid(std::string_view druid) {
    _request.set(http_::request_druid_hdr, druid);
}

//----

void PublicInjectorRequest::set_druid(std::string_view druid) {
    std::visit(
        [&] (auto&& alt) { alt.set_druid(druid); },
        static_cast<Base&>(*this)
    );
}

http::verb PublicInjectorRequest::method() const {
    return std::visit(
        [] (const auto& alt)
        { return alt.method(); },
        static_cast<const Base&>(*this)
    );
}

bool PublicInjectorRequest::is_inject_request() const {
    return std::get_if<CacheInjectRequest>(static_cast<const Base*>(this)) != nullptr;
}

} // namespace ouinet
