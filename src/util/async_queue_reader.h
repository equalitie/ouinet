#pragma once

#include "response_reader.h"
#include "async_queue.h"

namespace ouinet {

class AsyncQueueReader : public http_response::AbstractReader {
public:
    using Part = http_response::Part;
    using Queue = util::AsyncQueue<std::optional<Part>>;

    AsyncQueueReader(Queue& q)
        : _queue(q)
    {}

    std::expected<std::optional<Part>, sys::error_code>
    async_read_part(Async yield) override {
        auto c = _cancel.connect([&] { yield.cancel(); });

        auto part_e = compat([&](Cancel cancel, asio::yield_context yield) {
            return _queue.async_pop(cancel, yield);
        })(yield);

        if (!part_e) {
            _cancel(); // Indicate we're done
            return std::unexpected(part_e.error());
        }

        auto part = std::move(*part_e);
        if (!part) {
            _is_done = true;
            _cancel(); // Indicate we're done
        }

        return part;
    }

    bool is_done() const override
    {
        return _is_done;
    }

    void insert(Part p) {
        _queue.push_back(std::move(p));
    }

    void close() override {
        _queue.push_back(std::nullopt);
        _cancel();
    }

    ~AsyncQueueReader() {
        _cancel();
    }

    AsioExecutor get_executor() override
    {
        return _queue.get_executor();
    }

private:
    Cancel _cancel;
    Queue& _queue;
    bool _is_done = false;
};

}
