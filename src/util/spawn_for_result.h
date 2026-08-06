#pragma once

#include "namespaces.h"
#include "async.h"
#include "util/wait_condition.h"
#include "util/overloaded.h"

#include <boost/asio/spawn.hpp>
#include <variant>

namespace ouinet {

//--------------------------------------------------------------------

template<class V> class MappedTaskHandle;

//--------------------------------------------------------------------

namespace detail {
    template<class T> struct RefOrVoid       { using type = T&;   };
    template<>        struct RefOrVoid<void> { using type = void; };
    template<class T> struct ConstRefOrVoid       { using type = const T&; };
    template<>        struct ConstRefOrVoid<void> { using type = void;     };
}

//--------------------------------------------------------------------

template<class V> class TaskHandle {
public:
    using StoredV = std::conditional_t<std::is_void_v<V>, std::monostate, V>;
    using Result = std::variant<StoredV, std::exception_ptr>;
    template<class T> using Ref = detail::RefOrVoid<T>::type;
    template<class T> using ConstRef = detail::ConstRefOrVoid<T>::type;

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

        bool has_result() const {
            return std::visit(overloaded {
                    [] (const WaitCondition::Lock&) { return false; },
                    [] (const Result&) { return true; }
                },
                state);
        }

        Ref<V> wait_ref(Async yield) {
            auto slot = cancel.connect([&] { yield.cancel(); });

            if (!has_result()) {
                wc.wait(yield);
            }

            return std::visit(overloaded {
                    [] (V& value) -> V& { return value; },
                    [] (const std::exception_ptr& eptr) -> V& { std::rethrow_exception(eptr); }
                },
                std::get<Result>(state));
        }

        ConstRef<V> get_result_ref() const requires (!std::is_void_v<V>) {
            if (!has_result()) {
                throw std::runtime_error("Called TaskHandle::get_result() on unfinished task");
            }

            return std::visit(overloaded {
                    [] (const V& value) -> const V& { return value; },
                    [] (const std::exception_ptr& eptr) -> const V& { std::rethrow_exception(eptr); }
                },
                std::get<Result>(state));
        }

        ~Shared() {
            cancel();
        }

        WaitCondition wc;
        Cancel cancel;
        State state;
    };

public:
    V wait(Async yield) {
        return _shared->wait_ref(yield);
    }

    Ref<V> wait_ref(Async yield) {
        return _shared->wait_ref(yield);
    }

    ConstRef<V> get_result_ref() const {
        return _shared->get_result_ref();
    }

    bool has_result() const {
        return _shared->has_result();
    }

    template<class MapFunc>
    [[nodiscard]]
    MappedTaskHandle<
        std::invoke_result_t<MapFunc, V>
    >
    map(MapFunc map_func) const;

    // Same state as when moved from
    TaskHandle() = default;

    void cancel() {
        _shared->cancel();
    }

    void join(Async yield) {
        _shared->join(yield);
    }

private:
    template<class F>
    friend TaskHandle<std::invoke_result_t<F, Async>>
    spawn_for_result(asio::any_io_executor, Cancel, util::LogPath, F);

    template<class> friend class MappedTaskHandle;

    TaskHandle(std::shared_ptr<Shared> shared): _shared(std::move(shared)) {}

    std::shared_ptr<Shared> _shared;
};

//--------------------------------------------------------------------

template<class MV> class MappedTaskHandle {
private:
    struct SharedBase {
        virtual MV wait(Async yield) = 0;
        virtual bool has_result() const = 0;
        virtual ~SharedBase() = default;
    };

public:
    MV wait(Async yield) {
        return _shared->wait(yield);
    }

    bool has_result() const {
        return _shared->has_result();
    }

private:
    template<class> friend class TaskHandle;

    std::shared_ptr<SharedBase> _shared;
};

//--------------------------------------------------------------------

template<class V>
template<class MapFunc>
MappedTaskHandle<
    std::invoke_result_t<MapFunc, V>
>
TaskHandle<V>::map(MapFunc map_func) const {
    using MV = std::invoke_result_t<MapFunc, V>;

    struct Shared : MappedTaskHandle<MV>::SharedBase {
        MV wait(Async async) override {
            return _map_func(_inner->wait_ref(async));
        }

        bool has_result() const override {
            return _inner->has_result();
        }

        std::shared_ptr<TaskHandle<V>::Shared> _inner;
        MapFunc _map_func;
    };

    return MappedTaskHandle<MV>(std::make_shared<Shared>(_shared, std::move(map_func)));
}

//--------------------------------------------------------------------

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
            // Note that the following line of code may not be executed right
            // after the `asio::spawn` call depending on whether or not
            // `asio::spawn` was called from the same `exec`utor. So we have
            // to check for cancellation.
            if (cancel) throw Async::Cancelled();

            if constexpr (std::is_void_v<V>) {
                func(Async(y, cancel, log_path));

                if (auto shared = weak.lock()) {
                    shared->state = std::monostate{};
                }
            }
            else {
                V value = func(Async(y, cancel, log_path));

                if (auto shared = weak.lock()) {
                    shared->state = std::move(value);
                }
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

    return R(std::move(shared));
}

} // namespace
