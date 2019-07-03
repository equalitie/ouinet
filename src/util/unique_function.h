#pragma once

namespace ouinet { namespace util {

/*
 * A version of std::function whose wrapped Callable need not be
 * copy-constructible, but only move constructible. Convenient for storing
 * asio handlers, which are not necessarily copy constructible.
 *
 * Does not have all the bells and whistles of std::function.
 * Feel free to add them.
 *
 * Shamelessly based on:
 * https://stackoverflow.com/questions/28179817/how-can-i-store-generic-packaged-tasks-in-a-container
 */

template<class T> class unique_function;

// TODO: Small object allocator optimization
template<class Result, class... Args>
class unique_function<Result(Args...)> {
    public:
    unique_function() {}

    unique_function(std::nullptr_t) {}
    unique_function& operator=(std::nullptr_t)
    {
        _impl = nullptr;
        return *this;
    }

    unique_function(const unique_function&) = delete;
    unique_function& operator=(const unique_function&) = delete;

    unique_function(unique_function&&) = default;
    unique_function& operator=(unique_function&&) = default;

    template<class F>
    unique_function(F&& f):
        _impl(std::make_unique<impl<F>>( std::forward<F>(f) ))
    {}

    operator bool() const
    {
        return (bool)_impl;
    }

    Result operator()(Args... args)
    {
        return _impl->invoke(std::forward<Args>(args)...);
    }

    private:
    struct impl_base {
        virtual ~impl_base() {}
        virtual Result invoke(Args&&... args) = 0;
    };

    template<class F>
    struct impl : public impl_base {
        F _f;

        impl(F&& f):
            _f(std::move(f))
        {}

        Result invoke(Args&&... args) override
        {
            return _f(std::forward<Args>(args)...);
        }
    };

    private:
    std::unique_ptr<impl_base> _impl;
};

}} // namespaces
