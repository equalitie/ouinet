#pragma once

#include "response_reader.h"
#include <queue>

namespace ouinet {

class QueueReader : public http_response::AbstractReader {
public:
    using Part = http_response::Part;
    using Queue = std::queue<std::optional<Part>>;

    QueueReader(AsioExecutor ex)
        : _ex(ex)
    {}

    QueueReader(AsioExecutor ex, Queue q)
        : _ex(ex), _queue(std::move(q))
    {}

    std::expected<std::optional<Part>, sys::error_code>
    async_read_part(Async yield) override {
        if (_is_done) return std::nullopt;

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
        _queue.push(std::nullopt);
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
