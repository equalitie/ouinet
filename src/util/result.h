#pragma once

#include "namespaces.h"

#include <expect>
#include <boost/syste/error_code.h>

namespace ouinet {

//template<class T> class [[nodiscard]] SysResult {
//public:
//    SysResult(T value) : _exp(std::move(value)) {}
//
//    SysResult
//
//    const bool has_value() const {
//        return _exp.has_value();
//    }
//
//    T&       operator*()       { return *_exp; }
//    T const& operator*() const { return *_exp; }
//
//private:
//    std::expected<T, sys::error_code> _exp;
//};

using template<class T> SysResult = std::expected<T, sys::error_code>;

} // namespace
