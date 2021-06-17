#pragma once

#include "../response_part.h"
#include <ostream>

namespace ouinet { namespace http_response {

inline
std::ostream& operator<<(std::ostream& o, const http_response::Head& h) {
    o << "Head";
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::ChunkHdr& c) {
    o << "ChunkHdr size:" << c.size << " exts:" << c.exts;
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::ChunkBody& b) {
    o << "ChunkBody size:" << b.size() << " remain:" << b.remain;
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::Body& b) {
    o << "Body";
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::Trailer& t) {
    o << "Trailer";
    bool first = true;
    for (auto& f : t) {
        if (!first) o << ";";
        first = false;
        o << " " << f.name_string() << ":" << f.value();
    }
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::Part& p) {
    util::apply(p, [&](const auto& p_) { o << p_; });
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const http_response::Part::Type& t) {
    using Type = http_response::Part::Type;

    switch (t) {
        case Type::HEAD:       o << "HEAD";       break;
        case Type::BODY:       o << "BODY";       break;
        case Type::CHUNK_HDR:  o << "CHUNK_HDR";  break;
        case Type::CHUNK_BODY: o << "CHUNK_BODY"; break;
        case Type::TRAILER:    o << "TRAILER";    break;
    }

    return o;
}

}}
