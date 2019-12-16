#include "namespaces.h"
#include "response_part.h"

#include <boost/container/flat_map.hpp>

namespace ouinet { namespace http_response {

static
boost::container::flat_map<boost::string_view, boost::string_view>
fields_to_map(const http::fields& fields) {
    using boost::container::flat_map;
    using boost::string_view;
    using Map = flat_map<string_view, string_view>;
    Map ret;
    ret.reserve(std::distance(fields.begin(), fields.end()));
    for (auto& f : fields) {
        ret.insert(Map::value_type(f.name_string(), f.value()));
    }
    return ret;
}

using asio::yield_context;

bool Head::operator==(const Head& other) const {
    if (version() != other.version()) return false;
    if (result_int() != other.result_int()) return false;
    return fields_to_map(*this) == fields_to_map(other);
}

bool Trailer::operator==(const Trailer& other) const {
    return fields_to_map(*this) == fields_to_map(other);
}

}} // namespace ouinet::http_response
