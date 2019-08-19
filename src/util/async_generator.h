#pragma once

namespace ouinet { namespace util {

// Class to asynchronously generate values.
//
// Usage:
//
// //----------------------------------------------------------------
// AsyncGenerator gen(ioc, [&] (auto& queue, auto cancel, auto yield) {
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
    using Queue = AsyncQueue<Value>;
    using Yield = asio::yield_context;

public:
    template<class Generator /* void(Queue&, Cancel, Yield) */>
    AsyncGenerator(asio::io_context& ioc, Generator&& gen)
        : _queue(ioc)
        , _shutdown_cancel(_lifetime_cancel)
        , _wc(ioc)
    {
        asio::spawn(ioc, [ &
                         , gen = std::move(gen)
                         , lifetime_cancel = _lifetime_cancel
                         , shutdown_cancel = _shutdown_cancel
                         , lock = _wc.lock()
                         ] (Yield yield) mutable {
            sys::error_code ec;
            gen(_queue, shutdown_cancel, yield[ec]);

            // lifetime_cancel => shutdown_cancel
            assert(!lifetime_cancel || shutdown_cancel);

            // shutdown_cancel => operation_aborted
            assert(!shutdown_cancel || ec == asio::error::operation_aborted);

            if (!lifetime_cancel) _last_ec = ec;
        });
    }

    boost::optional<Value> async_get_value(Cancel cancel, Yield yield) {
        // Return none if the coroutine is no longer running
        if (_last_ec) { return boost::none; }

        auto c = _shutdown_cancel.connect([&] { cancel(); });
        return _queue.async_pop(cancel, yield);
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
