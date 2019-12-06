#pragma once

#include <boost/optional.hpp>
#include "async_queue.h"
#include "wait_condition.h"

namespace ouinet { namespace util {

// Class to asynchronously generate values.
//
// Usage:
//
// //----------------------------------------------------------------
// AsyncGenerator gen(ex, [&] (auto& queue, auto cancel, auto yield) {
//     unsigned n = 0;
//     while (!cancel) {
//         sleep(ios, 1s, cancel, yield);
//         if (cancel) break;
//         q.push_back(n++);
//     }
// });
//
// sys::error_code ec;
// while (auto opt_val = gen.async_get_value(cancel, yield[ec])) {
//     std::cout << *opt_val << "\n";
// }
// //----------------------------------------------------------------
//
// Output:
// 0
// 1
// 2
// 3
// ...
//
//

template<class Value> class AsyncGenerator {
private:
    using OptValue = boost::optional<Value>;
    using Queue = AsyncQueue<OptValue>;
    using Yield = asio::yield_context;

public:
    template<class Generator /* void(Queue&, Cancel, Yield) */>
    AsyncGenerator(asio::io_context& ctx, Generator&& gen)
        : AsyncGenerator(ctx.get_executor(), std::forward<Generator>(gen))
    {}

    template<class Generator /* void(Queue&, Cancel, Yield) */>
    AsyncGenerator(const asio::executor& ex, Generator&& gen)
        : _queue(ex)
        , _shutdown_cancel(_lifetime_cancel)
        , _wc(ex)
    {
        asio::spawn(ex, [ self = this
                        , gen = std::move(gen)
                        , lifetime_cancel = _lifetime_cancel
                        , shutdown_cancel = _shutdown_cancel
                        , lock = _wc.lock()
                        ] (Yield yield) mutable {
            sys::error_code ec;
            gen(self->_queue, shutdown_cancel, yield[ec]);

            // lifetime_cancel => shutdown_cancel
            assert(!lifetime_cancel || shutdown_cancel);

            // shutdown_cancel => operation_aborted
            assert(!shutdown_cancel || ec == asio::error::operation_aborted);

            if (!lifetime_cancel) {
                self->_last_ec = shutdown_cancel
                               ? asio::error::operation_aborted
                               : ec;

                self->_queue.push_back(boost::none);
            }
        });
    }

    boost::optional<Value> async_get_value(Cancel cancel, Yield yield) {
        using Ret = boost::optional<Value>;

        if (_shutdown_cancel) {
            return or_throw<Ret>(yield, asio::error::operation_aborted);
        }

        if (_queue.size()) {
            OptValue v = std::move(_queue.front());
            _queue.pop();
            return v;
        }

        // Return none if the coroutine is no longer running
        if (_last_ec) { return boost::none; }

        Cancel lfc = _lifetime_cancel;
        auto c = _shutdown_cancel.connect([&] { cancel(); });

        sys::error_code ec; // ignored
        auto ret = _queue.async_pop(cancel, yield[ec]);

        if (lfc || _shutdown_cancel) {
            return or_throw<Ret>(yield, asio::error::operation_aborted);
        }

        return ret;
    }

    void async_shut_down(Yield yield) {
        if (!_shutdown_cancel) _shutdown_cancel();
        _wc.wait(yield);
    }

    ~AsyncGenerator() {
        if (!_lifetime_cancel) _lifetime_cancel();
    }

    boost::optional<sys::error_code> last_error() const {
        return _last_ec;
    }

private:
    Queue _queue;
    Cancel _lifetime_cancel;
    Cancel _shutdown_cancel;
    WaitCondition _wc;
    boost::optional<sys::error_code> _last_ec;
};


}} // namespaces
