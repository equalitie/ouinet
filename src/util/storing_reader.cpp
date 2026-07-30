#include "storing_reader.h"
#include "session.h"
#include "request.h"
#include "cache/client.h"
#include "cache/resource_id.h"
#include "util/async_queue.h"
#include "util/async_queue_reader.h"
#include "logger.h"
#include "util/debug.h"

namespace ouinet {

using Part = StoringReader::Part;
using Queue = util::AsyncQueue<std::optional<Part>>;

struct StoringReader::Impl : std::enable_shared_from_this<Impl> {
    Cancel cancel;
    Queue queue;
    Session session;
    std::shared_ptr<cache::Client> cache;
    std::string dht_group;
    cache::ResourceId resource_id;
    bool is_done;

    Impl(const CacheRequest& rq, Session session, std::shared_ptr<cache::Client> cache):
        queue(session.get_executor(), 1),
        session(std::move(session)),
        cache(std::move(cache)),
        dht_group(rq.dht_group()),
        resource_id(rq.resource_id()),
        is_done(false)
    {
    }

    void start() {
        task::spawn_detached(session.get_executor(), [self = shared_from_this()] (asio::yield_context y) {
            // Cancel is unused because if the `StoringReader`'s instance is
            // destroyed, we still want to write whatever has been pushed into
            // the queue.
            Cancel unused_cancel;
            Async yield(y, unused_cancel);
            AsyncQueueReader queue_reader(self->queue);
            auto r = self->cache->store(self->resource_id, self->dht_group, queue_reader, yield);
            if (!r) LOG_ERROR(yield, " Failed to write response to cache; ec=", r.error());
        });
    }

    std::expected<std::optional<Part>, sys::error_code> async_read_part(Async yield) {
        if (is_done) {
            return std::nullopt;
        }
    
        auto c = cancel.connect([&] { yield.cancel(); });
    
        auto part = session.async_read_part(yield);
    
        if (!part) {
            queue.push_back(std::nullopt);
            return part;
        }
    
        std::ignore = queue.async_push(*part, yield);

        if (!*part) {
            is_done = true;
        }
    
        return part;
    }
};

StoringReader::StoringReader(const CacheRequest& rq, Session session, std::shared_ptr<cache::Client> cache)
    : _impl(std::make_shared<Impl>(rq, std::move(session), std::move(cache)))
{
    _impl->start();
}

std::expected<
    std::optional<Part>,
    sys::error_code
>
StoringReader::async_read_part(Async yield) {
    if (!_impl) return std::unexpected(asio::error::fault);
    return _impl->async_read_part(yield);
}

bool StoringReader::is_done() const {
    if (!_impl) std::terminate();
    return _impl->is_done;
}

void StoringReader::close() {
    if (!_impl) return;
    _impl->queue.push_back(std::nullopt);
    _impl->session.close();
    _impl->cancel();
}

asio::any_io_executor StoringReader::get_executor() {
    if (!_impl) std::terminate();
    return _impl->session.get_executor();
}

StoringReader::~StoringReader() {
    close();
}

StoringReader::StoringReader(StoringReader&&) = default;
StoringReader& StoringReader::operator=(StoringReader&&) = default;


} // namespace
