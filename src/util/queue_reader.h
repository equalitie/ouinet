#pragma once

#include "response_reader.h"
#include <queue>

namespace ouinet {

class QueueReader : public http_response::AbstractReader {
public:
    using Part = http_response::Part;
    using Queue = std::queue<boost::optional<Part>>;

    QueueReader(AsioExecutor ex)
        : _ex(ex)
    {}

    QueueReader(AsioExecutor ex, Queue q)
        : _ex(ex), _queue(std::move(q))
    {}

    boost::optional<Part> async_read_part(Cancel cancel, asio::yield_context yield) override {
        if (cancel) return or_throw<Part>(yield, asio::error::operation_aborted);
        if (_is_done) return boost::none;

        assert(!_queue.empty());
        auto opt_p = std::move(_queue.front());
        _queue.pop();
        if (!opt_p || _queue.empty()) _is_done = true;

        return opt_p;
    }

    bool is_done() const override
    {
        return _is_done;
    }

    void insert(Part p) {
        _queue.push(std::move(p));
    }

    void close() override {
        _queue.push(boost::none);
    }

    AsioExecutor get_executor() override
    {
        return _ex;
    }

private:
    AsioExecutor _ex;
    Queue _queue;
    bool _is_done = false;
};

}
