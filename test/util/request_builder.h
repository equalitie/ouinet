#pragma once

#include "route.h"
#include "util/url.h"

#include <boost/beast/http/string_body.hpp>

#include <optional>
#include <string>

namespace ouinet {

http::request<http::string_body> build_origin_request(const util::Url& url);

http::request<http::string_body> build_private_request(const util::Url& url);

struct CacheRequestBuilder {
    util::Url url;
    std::optional<std::string> resource_group;
    std::optional<Route> route;

    CacheRequestBuilder(util::Url url) : url(std::move(url)) {}

    CacheRequestBuilder& set_resource_group(std::string resource_group) {
        this->resource_group = resource_group;
        return *this;
    }

    CacheRequestBuilder& set_route(Route route) {
        this->route = route;
        return *this;
    }

    http::request<http::string_body> build() const;
};

} // namespace
