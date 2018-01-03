#pragma once

#include <boost/variant.hpp>
#include "namespaces.h"

namespace ouinet {

template<class Value>
class Result {
public:
    template<class T> Result(T&& r);

    bool is_error() const;
    bool is_value() const;

    Value& operator*();
    const Value& operator*() const;

    Value* operator->();
    const Value* operator->() const;

    sys::error_code get_error() const;

    operator bool() const;

    static Result<Value> make_error(const sys::error_code&);

private:
    boost::variant<Value, sys::error_code> _result;
};


template<class V>
template<class T>
Result<V>::Result(T&& r)
    : _result(std::forward<T>(r))
{}


template<class V>
Result<V> Result<V>::make_error(const sys::error_code& ec)
{
    return Result<V>{ec};
}

template<class V> bool Result<V>::is_error() const
{
    return boost::get<sys::error_code>(&_result) != nullptr;
}


template<class V> bool Result<V>::is_value() const
{
    return boost::get<V>(&_result) != nullptr;
}


template<class V> V& Result<V>::operator*()
{
    if (auto p = boost::get<V>(&_result)) {
        return *p;
    }

    throw std::runtime_error("Result: no value present");
}


template<class V> const V& Result<V>::operator*() const
{
    if (auto p = boost::get<V>(&_result)) {
        return *p;
    }

    throw std::runtime_error("Result: no value present");
}


template<class V> V* Result<V>::operator->()
{
    return &**this;
}


template<class V> const V* Result<V>::operator->() const
{
    return &**this;
}


template<class V> sys::error_code Result<V>::get_error() const
{
    if (auto p = boost::get<sys::error_code>(&_result)) {
        return *p;
    }

    return sys::error_code();
}


template<class V> Result<V>::operator bool() const
{
    return !get_error();
}

} // ouinet namespace
