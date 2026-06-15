#pragma once

#include "../defer.h"
#include "condition_variable.h"
#include <optional>

namespace ouinet {

template<class Retval> class AsyncJob {
public:
    using Job = std::function<Retval(Cancel&, asio::yield_context)>;
    using OnFinish = std::function<void()>;
    using Connection = typename Cancel::Connection;

    using Result = std::expected<Retval, sys::error_code>;

public:
    AsyncJob(const AsioExecutor& ex)
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
        task::spawn_detached(_ex, [s, job = std::move(job)]
                         (asio::yield_context yield) {
            AsyncJob* self = s;

            Cancel cancel;

            self->_self = &self;
            self->_cancel_signal = &cancel;

            Result result = compat([&](sys::error_code& ec) {
                return job(cancel, yield[ec]);
            })();
            if (cancel) {
                result = std::unexpected(asio::error::operation_aborted);
            }

            if (!self) return;

            self->_self = nullptr;
            self->_cancel_signal = nullptr;

            self->_result = std::move(result);

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

    std::optional<Connection> on_finish_sig(OnFinish on_finish)
    {
        if (!_self) {
            return std::nullopt;
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

    void wait_for_finish(Cancel& c, asio::yield_context yield) {
        auto con = c.connect([&] { cancel(); });
        sys::error_code ec;
        wait_for_finish(yield[ec]);
        return_or_throw_on_error(yield, c, ec);;
    }

    void cancel() {
        if (_cancel_signal) {
            (*_cancel_signal)();
            _cancel_signal = nullptr;
        }
    }

private:
    AsioExecutor _ex;
    std::optional<Result> _result;
    Cancel* _cancel_signal = nullptr;
    AsyncJob** _self = nullptr;
    Cancel _on_finish_sig;
};

} // namespace
