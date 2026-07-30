#pragma once

#include "namespaces.h"
#include "async.h"
#include "api.h"

#include <boost/asio/spawn.hpp>
#include <variant>

namespace ouinet {

template<class V> class TaskHandle {
public:
    using Result = std::variant<V, std::exception_ptr>;

private:
    struct Shared {
        using State = std::variant<
            WaitCondition::Lock,
            Result
        >;

        Shared(asio::any_io_executor exec) :
            wc(std::move(exec)),
            state(wc.lock())
        {}

        ~Shared() {
            cancel();
        }

        WaitCondition wc;
        Cancel cancel;
        State state;
    };

public:
    TaskHandle(TaskHandle const&) = default;
    TaskHandle(TaskHandle&&) = default;

    TaskHandle& operator=(TaskHandle const&) = default;
    TaskHandle& operator=(TaskHandle&&) = default;

    V wait(Async yield) {
        return wait_ref(yield);
    }

    V& wait_ref(Async yield) {
        auto slot = _shared->cancel.connect([&] { yield.cancel(); });

        auto& state = _shared->state;

        if (!has_result()) {
            _shared->wc.wait(yield);
        }

        return std::visit(overloaded {
                [&] (V& value) -> V& { return value; },
                [&] (const std::exception_ptr& eptr) -> V& { std::rethrow_exception(eptr); }
            },
            std::get<Result>(state));
    }

    const V& get_result_ref() const {
        auto& state = _shared->state;

        if (!has_result()) {
            throw std::runtime_error("Called TaskHandle::get_result() on unfinished task");
        }

        return std::visit(overloaded {
                [&] (V& value) -> V& { return value; },
                [&] (const std::exception_ptr& eptr) -> V& { std::rethrow_exception(eptr); }
            },
            std::get<Result>(state));
    }

    bool has_result() const {
        return std::visit(overloaded {
                [] (const WaitCondition::Lock&) { return false; },
                [] (const Result&) { return true; }
            },
            _shared->state);
    }

private:
    template<class F>
    friend TaskHandle<std::invoke_result_t<F, Async>>
    spawn_for_result(asio::any_io_executor, Cancel, util::LogPath, F);

    TaskHandle(std::shared_ptr<Shared> shared): _shared(std::move(shared)) {}

    std::shared_ptr<Shared> _shared;
};

template<class F>
[[nodiscard]]
TaskHandle<
    std::invoke_result_t<F, Async>
>
spawn_for_result(
        asio::any_io_executor exec,
        Cancel cancel,
        util::LogPath log_path,
        F func
) {
    using V = std::invoke_result_t<F, Async>;
    using R = TaskHandle<V>;
    using Shared = R::Shared;

    auto shared = std::make_shared<Shared>(exec);

    std::weak_ptr<Shared> weak = shared;

    asio::spawn(
        exec, [
            weak,
            cancel,
            func = std::move(func),
            log_path = std::move(log_path)
        ] (asio::yield_context y) {
            if (cancel) throw Async::Cancelled();

            V value = func(Async(y, cancel, log_path));

            if (auto shared = weak.lock()) {
                shared->state = std::move(value);
            }
        },
        [weak] (std::exception_ptr eptr) {
            if (!eptr) return;
           
            if (auto shared = weak.lock()) {
                std::visit(overloaded {
                        [&] (const WaitCondition::Lock&) {
                            shared->state = std::move(eptr);
                        },
                        [] (const R::Result&) {
                            std::unreachable();
                        }
                    },
                    shared->state);
            }
        });

    return R{ std::move(shared) };
}

} // namespace
