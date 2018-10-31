#pragma once

#include "../defer.h"
#include "condition_variable.h"

namespace ouinet {

template<class Retval> class AsyncJob {
public:
    using Job = std::function<Retval(Cancel&, asio::yield_context)>;
    using OnFinish = std::function<void()>;

    struct Result {
        sys::error_code ec;
        Retval retval;
    };

public:
    AsyncJob(asio::io_service& ios)
        : _ios(ios)
    {}

    AsyncJob(const AsyncJob&) = delete;

    AsyncJob(AsyncJob&& other)
        : _ios(other._ios)
        , _result(std::move(other._result))
        , _cancel_signal(other._cancel_signal)
        , _self(other._self)
        , _on_finish(std::move(other._on_finish))
    {
        if (_self) { *_self = this; }

        other._cancel_signal = nullptr;
        other._self = nullptr;
    }

    AsyncJob& operator=(AsyncJob&& other) {
        assert(&_ios == &other._ios);

        _result = std::move(other._result);
        _cancel_signal = other._cancel_signal;
        _on_finish = std::move(other._on_finish);

        if (other._self) *other._self = this;
        other._cancel_signal = nullptr;
        other._self = nullptr;

        return *this;
    }

    void start(Job job) {
        assert(!_self && "Already started");
        if (_self) return;

        AsyncJob* s = this;
        asio::spawn(_ios, [s, job = std::move(job)]
                          (asio::yield_context yield) {
            AsyncJob* self = s;

            Signal<void()> cancel;

            self->_self = &self;
            self->_cancel_signal = &cancel;

            auto on_exit = defer([&] {
                if (self == nullptr) return;
                self->_self = nullptr;
                self->_cancel_signal = nullptr;
            });

            sys::error_code ec;
            Retval retval = job(cancel, yield[ec]);

            if (!self) return;
            if (!ec && cancel) ec = asio::error::operation_aborted;

            self->_result = Result{ ec, std::move(retval) };

            if (self->_on_finish) {
                auto on_finish = std::move(self->_on_finish);
                on_finish();
            }
        });
    }

    ~AsyncJob() {
        if (_self) *_self = nullptr;
        if (_cancel_signal) (*_cancel_signal)();
    }

    bool has_result() const {
        return bool(_result);
    }

    const Result&  result() const& { return *_result; }
          Result&  result() &      { return *_result; }
          Result&& result() &&     { return std::move(*_result); }

    void on_finish(OnFinish on_finish)
    {
        assert((_self || _result) && "Job is/was not running");

        if (!_self) {
            if (on_finish) _ios.post(std::move(on_finish));
        }
        else {
            _on_finish = std::move(on_finish);
        }
    }

    bool is_running() const { return _self; }

    void stop(asio::yield_context yield) {
        if (!_self) return;
        assert(!_on_finish);
        assert(_cancel_signal);
        (*_cancel_signal)();
        _cancel_signal = nullptr;
        ConditionVariable cv(_ios);
        _on_finish = [&cv] { cv.notify(); };
        cv.wait(yield);
    }

private:
    asio::io_service& _ios;
    boost::optional<Result> _result;
    Signal<void()>* _cancel_signal = nullptr;
    AsyncJob** _self = nullptr;
    OnFinish _on_finish;
};

} // namespace
