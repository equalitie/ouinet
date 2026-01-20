#include "namespaces.h"
#include "response_part.h"

#include <boost/container/flat_map.hpp>

namespace ouinet::http_response {

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

std::ostream& operator<<(std::ostream& os, Part::Type type) {
    switch (type) {
        case Part::Type::HEAD: return os << "HEAD";
        case Part::Type::BODY: return os << "BODY";
        case Part::Type::CHUNK_HDR: return os << "CHUNK_HDR";
        case Part::Type::CHUNK_BODY: return os << "CHUNK_BODY";
        case Part::Type::TRAILER: return os << "CHUNK_TRAILER";
        default: return os << "?";
    }
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::Part const& part) {
    os << "Part{";
    ouinet::util::apply(part, [&](const auto& p) { os << p; });
    return os << "}";
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::Head const&) {
    return os << "Head{ ... }";
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::ChunkHdr const&) {
    return os << "ChunkHdr{ ... }";
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::ChunkBody const& part) {
    return os << "ChunkBody{ size:" << part.size() << ", ... }";
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::Body const& part) {
    return os << "Body{ size:" << part.size() << ", ... }";
}

std::ostream& operator<<(std::ostream& os, ouinet::http_response::Trailer const&) {
    return os << "Trailer{ ... }";
}

} // namespace ouinet::http_response
