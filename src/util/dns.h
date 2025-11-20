#pragma once

#include <iterator>

namespace ouinet::util
{
    // Transforms addresses to endpoints with the given port.
    template <class Addrs, class Endpoint>
    class AddrsAsEndpoints
    {
    public:
        using value_type = Endpoint;
        using addrs_iterator = typename Addrs::const_iterator;

        AddrsAsEndpoints(const Addrs& addrs, unsigned short port)
            : _addrs(addrs), _port(port)
        {
        }

        class const_iterator
        {
        public:
            // Iterator requirements
            using iterator_category = std::input_iterator_tag;
            using value_type = Endpoint;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            const_iterator(const addrs_iterator& it, unsigned short port)
                : _it(it), _port(port)
            {
            }

            value_type operator*() const { return {*_it, _port}; }

            const_iterator& operator++()
            {
                ++_it;
                return *this;
            }

            bool operator==(const const_iterator& other) const { return _it == other._it; }
            bool operator!=(const const_iterator& other) const { return _it != other._it; }

        private:
            addrs_iterator _it;
            unsigned short _port;
        };

        const_iterator begin() const { return {_addrs.begin(), _port}; };
        const_iterator end() const { return {_addrs.end(), _port}; };

    private:
        const Addrs& _addrs;
        unsigned short _port;
    };

    inline
    auto tcp_async_resolve( const std::string& host
                          , const std::string& port
                          , AsioExecutor exec
                          , Cancel& cancel
                          , asio::yield_context yield)
    {
        using tcp = asio::ip::tcp;
        using Results = tcp::resolver::results_type;

        if (cancel) {
            return or_throw<Results>(yield, asio::error::operation_aborted);
        }

        // Note: we're spawning a new coroutine here and deal with all this
        // ConditionVariable machinery because - contrary to what Asio's
        // documentation says - resolver::async_resolve isn't immediately
        // cancelable. I.e.  when resolver::async_resolve is running and
        // resolver::cancel is called, it is not guaranteed that the async_resolve
        // call gets placed on the io_service queue immediately. Instead, it was
        // observed that this can in some rare cases take more than 20 seconds.
        //
        // Also note that this is not Asio's fault. Asio uses internally the
        // getaddrinfo() function which doesn't support cancellation.
        //
        // https://stackoverflow.com/questions/41352985/abort-a-call-to-getaddrinfo
        sys::error_code ec;
        Results results;
        ConditionVariable cv(exec);
        tcp::resolver* rp = nullptr;

        auto cancel_lookup_slot = cancel.connect([&] {
            ec = asio::error::operation_aborted;
            cv.notify();
            if (rp) rp->cancel();
        });

        bool* finished_p = nullptr;

        TRACK_SPAWN(exec, ([&] (asio::yield_context yield) {
            bool finished = false;
            finished_p = &finished;

            tcp::resolver resolver{exec};
            rp = &resolver;
            sys::error_code ec_;
            auto r = resolver.async_resolve(host, port, yield[ec_]);
            // Turn this confusing resolver error into something more understandable.
            static const sys::error_code busy_ec{ sys::errc::device_or_resource_busy
                                                , sys::system_category()};
            if (ec_ == busy_ec) ec_ = asio::error::host_not_found;

            if (finished) return;

            rp = nullptr;
            results = std::move(r);
            ec = ec_;
            finished_p = nullptr;
            cv.notify();
        }));

        cv.wait(yield);

        if (finished_p) *finished_p = true;

        ec = compute_error_code(ec, cancel);
        return or_throw(yield, ec, std::move(results));
    }
}
