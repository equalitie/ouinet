#pragma once

#include "response_reader.h"
#include "async_queue.h"

namespace ouinet {

class AsyncQueueReader : public http_response::AbstractReader {
public:
    using Part = http_response::Part;
    using Queue = util::AsyncQueue<boost::optional<Part>>;

    AsyncQueueReader(Queue& q)
        : _queue(q)
    {}

    boost::optional<Part> async_read_part(Cancel cancel, asio::yield_context yield) override {
        if (_cancel) return boost::none;
        auto c = _cancel.connect([&] { cancel(); });
        sys::error_code ec;
        auto opt_p = _queue.async_pop(cancel, yield[ec]);
        if (ec || !opt_p) {
            _is_done = !ec;
            _cancel(); // Indicate we're done
            return or_throw(yield, ec, boost::none);
        }
        return opt_p;
    }

    bool is_done() const override
    {
        return _is_done;
    }

    void insert(Part p) {
        _queue.push_back(std::move(p));
    }

    void close() override {
        _queue.push_back(boost::none);
        _cancel();
    }

    ~AsyncQueueReader() {
        _cancel();
    }

    asio::executor get_executor() override
    {
        return _queue.get_executor();
    }

private:
    Cancel _cancel;
    Queue& _queue;
    bool _is_done = false;
};

}
