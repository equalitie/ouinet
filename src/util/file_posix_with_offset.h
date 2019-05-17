#pragma once

#include <boost/beast/core/file_posix.hpp>

#include "../namespaces.h"

namespace ouinet { namespace util {

// A variant of the POSIX file which allows setting a *base offset*,
// so that bytes before that offset are hidden away and not available for
// reading, writing, seeking or computing the file size.
//
// The base offset must always lie within the current boundaries of the file.
//
// Setting the offset multiple times will move the fake beginning of the file
// further towards the end of the file.
class file_posix_with_offset : public beast::file_posix {
    uint64_t base_offset_ = 0;

public:
    uint64_t base_offset() const { return base_offset_; }
    void base_offset(uint64_t offset, sys::error_code& ec) {
        bool too_far = offset > size(ec);
        if (ec) return;
        if (too_far) { ec = asio::error::invalid_argument; return; }
        base_offset_ = offset;
    }

    uint64_t size(sys::error_code& ec) const {
        uint64_t ret = beast::file_posix::size(ec);
        if (ec) return ret;
        return ret - base_offset_;
    }

    uint64_t pos(sys::error_code& ec) const {
        uint64_t ret = beast::file_posix::pos(ec);
        if (ec) return ret;
        return ret - base_offset_;
    }

    void seek(std::uint64_t offset, sys::error_code& ec) {
        beast::file_posix::seek(base_offset_ + offset, ec);
    }
};

}} // namespaces
