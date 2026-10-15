#pragma once

#include "util/async.h"
#include "util/condition_variable.h"
#include <optional>
#include <memory>

namespace ouinet {

// See `test_watch.cpp` for examples
template<class T> class Watch {
public:
    class Producer;

private:
    struct Shared {
        uint64_t value_version = 0;
        T value;
        size_t producer_rc = 0;
        ConditionVariable cv;

        Shared(asio::any_io_executor exec, T value):
            value(std::move(value)),
            cv(exec)
        {}

        const T* await_change(std::optional<uint64_t> version_since, Async yield) {
            while (true) {
                if (value_version > version_since) {
                    return &value;
                }
                if (producer_rc == 0) {
                    return nullptr;
                }
                if (auto r = cv.wait(yield); !r) {
                    std::unreachable();
                }
            }
        }
    };

public:
    class Producer {
    public:
        Producer(asio::any_io_executor exec, T initial): 
            _shared(std::make_shared<Shared>(std::move(exec), std::move(initial)))
        {
            ++_shared->producer_rc;
        }

        Producer(std::shared_ptr<Shared> shared):
            _shared(std::move(shared))
        {
            ++_shared->producer_rc;
        }

        template<class V> void send(V&& new_value) {
            _shared->value = std::forward<V>(new_value);
            ++_shared->value_version;
            _shared->cv.notify();
        }

        // TODO if needed
        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;

        Producer(Producer&& other) {
            *this = std::move(other);
        }

        Producer& operator=(Producer&& other) {
            _shared = std::move(other._shared);
            other._shared.reset();
            return *this;
        }

        ~Producer() {
            if (!_shared) return; // moved from
            --_shared->producer_rc;
            _shared->cv.notify();
        }

        const T& value() const {
            return _shared->value;
        }

    private:
        friend class Watch;
        std::shared_ptr<Shared> _shared;
    };

    static
    std::pair<Producer, Watch> create(asio::any_io_executor exec, T initial) {
        Producer producer(std::move(exec), std::move(initial));
        Watch watch(producer);
        return std::make_pair(std::move(producer), std::move(watch));
    }

    const T& value() const {
        return _shared->value;
    }

    // Returns `nullptr` when the last producer is destroyed.
    const T* await_change(Async yield) {
        auto ret = _shared->await_change(_last_seen_version, yield);
        _last_seen_version = _shared->value_version;
        return ret;
    }

    Watch(const Producer& producer) : _shared(producer._shared) {}

private:
    Watch(std::shared_ptr<Shared> shared): _shared(std::move(shared)) {}

    std::optional<uint64_t> _last_seen_version;
    std::shared_ptr<Shared> _shared;
};

} // namespace
