#pragma once

#include "../response_part.h"
#include <ostream>

namespace ouinet { namespace util {

template<class T>
struct DebugBrief {
    const T& t;
};

template<class T>
inline
DebugBrief<T> debug_brief(const T& t) { return DebugBrief<T>{t}; }

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::Head>& h) {
    o << "Head";
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::ChunkHdr>& d) {
    auto& c = d.t;
    o << "ChunkHdr size:" << c.size << " exts:" << c.exts;
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::ChunkBody>& b) {
    o << "ChunkBody size:" << b.t.size() << " remain:" << b.t.remain;
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::Body>& b) {
    o << "Body";
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::Trailer>& b) {
    o << "Trailer";
    bool first = true;
    for (auto& f : b.t) {
        if (!first) o << ";";
        first = false;
        o << " " << f.name_string() << ":" << f.value();
    }
    return o;
}

inline
std::ostream& operator<<(std::ostream& o, const DebugBrief<http_response::Part>& p) {
    util::apply(p.t, [&](const auto& p_) { o << debug_brief(p_); });
    return o;
}

}}
