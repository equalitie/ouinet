#pragma once

#include "../defer.h"
#include "condition_variable.h"

namespace ouinet {

template<class Retval> class AsyncJob {
public:
    using Job = std::function<Retval(Cancel&, asio::yield_context)>;
    using OnFinish = std::function<void()>;
    using OnFinishSig = Signal<void()>;
    using Connection = typename OnFinishSig::Connection;

    struct Result {
        sys::error_code ec;
        Retval retval;
    };

public:
    AsyncJob(const asio::executor& ex)
        : _ex(ex)
    {}

    AsyncJob(const AsyncJob&) = delete;

    AsyncJob(AsyncJob&& other)
        : _ex(std::move(other._ex))
        , _result(std::move(other._result))
        , _cancel_signal(other._cancel_signal)
        , _self(other._self)
        , _on_finish_sig(std::move(other._on_finish_sig))
    {
        if (_self) { *_self = this; }

        other._cancel_signal = nullptr;
        other._self = nullptr;
    }

    AsyncJob& operator=(AsyncJob&& other) {
        _result = std::move(other._result);
        _cancel_signal = other._cancel_signal;
        _on_finish_sig = std::move(other._on_finish_sig);

        _self = other._self;
        if (_self) *_self = this;
        other._cancel_signal = nullptr;
        other._self = nullptr;

        return *this;
    }

    void start(Job job) {
        assert(!_self && "Already started");
        if (_self) return;

        AsyncJob* s = this;
        asio::spawn(_ex, [s, job = std::move(job)]
                         (asio::yield_context yield) {
            AsyncJob* self = s;

            Signal<void()> cancel;

            self->_self = &self;
            self->_cancel_signal = &cancel;

            sys::error_code ec;
            Retval retval = job(cancel, yield[ec]);

            if (!self) return;

            self->_self = nullptr;
            self->_cancel_signal = nullptr;

            if (cancel) ec = asio::error::operation_aborted;

            self->_result = Result{ ec, std::move(retval) };

            auto on_finish_sig = std::move(self->_on_finish_sig);
            on_finish_sig();
        });
    }

    ~AsyncJob() {
        if (_self) *_self = nullptr;
        if (_cancel_signal) (*_cancel_signal)();
    }

    bool was_started() const {
        return is_running() || has_result();
    }

    bool has_result() const {
        return bool(_result);
    }

    const Result&  result() const& { return *_result; }
          Result&  result() &      { return *_result; }
          Result&& result() &&     { return std::move(*_result); }

    boost::optional<Connection> on_finish_sig(OnFinish on_finish)
    {
        if (!_self) {
            return boost::none;
        }
        else {
            return _on_finish_sig.connect(std::move(on_finish));
        }
    }

    bool is_running() const { return _self; }

    void stop(asio::yield_context yield) {
        if (!is_running()) return;
        cancel();
        ConditionVariable cv(_ex);
        auto con = _on_finish_sig.connect([&cv] { cv.notify(); });
        cv.wait(yield);
    }

    void wait_for_finish(asio::yield_context yield) {
        if (!is_running()) return;
        ConditionVariable cv(_ex);
        auto con = _on_finish_sig.connect([&cv] { cv.notify(); });
        cv.wait(yield);
    }

    void cancel() {
        if (_cancel_signal) {
            (*_cancel_signal)();
            _cancel_signal = nullptr;
        }
    }

private:
    asio::executor _ex;
    boost::optional<Result> _result;
    Signal<void()>* _cancel_signal = nullptr;
    AsyncJob** _self = nullptr;
    Signal<void()> _on_finish_sig;
};

} // namespace
