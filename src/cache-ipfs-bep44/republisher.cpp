#include "republisher.h"
#include <asio_ipfs.h>
#include <iostream>

#include "../logger.h"

using namespace std;
using namespace ouinet;

using Timer = asio::steady_timer;
using Clock = chrono::steady_clock;
static const Timer::duration publish_duration = chrono::minutes(10);

Republisher::Republisher(asio_ipfs::node& ipfs_node)
    : _was_destroyed(make_shared<bool>(false))
    , _ipfs_node(ipfs_node)
    , _timer(_ipfs_node.get_io_service())
{}

void Republisher::publish(const std::string& cid, asio::yield_context yield)
{
    using Handler = asio::handler_type< asio::yield_context
                                      , void(sys::error_code)>::type;

    Handler handler(move(yield));
    asio::async_result<Handler> result(handler);
    publish(cid, move(handler));
    result.get();
}

void Republisher::publish(const std::string& cid, std::function<void(sys::error_code)> cb)
{
    _to_publish = cid;

    _callbacks.push_back(move(cb));

    if (_is_publishing) {
        return;
    }

    start_publishing();
}

void Republisher::start_publishing()
{
    if (_callbacks.empty()) {
        _is_publishing = false;
        _timer.expires_from_now(publish_duration / 2);
        _timer.async_wait(
            [this, d = _was_destroyed] (sys::error_code ec) {
                if (*d) return;
                if (ec || _is_publishing) return;
                _callbacks.push_back(nullptr);
                start_publishing();
            });
        return;
    }

    _is_publishing = true;
    _timer.cancel();

    auto last_i = --_callbacks.end();

    cout << "Publishing DB: " << _to_publish << endl;
    _ipfs_node.publish(_to_publish, publish_duration,
        [this, d = _was_destroyed, last_i, id = _to_publish] (sys::error_code ec) {
            if (*d) return;
            LOG_DEBUG("Request was successfully published to cache under id " + id);

            while (true) {
                bool is_last = last_i == _callbacks.begin();
                auto cb = move(_callbacks.front());
                _callbacks.pop_front();
                if (cb) cb(ec);
                if (*d) return;
                if (is_last) break;
            }

            start_publishing();
        });
}

Republisher::~Republisher()
{
    *_was_destroyed = true;

    auto& ios = _ipfs_node.get_io_service();
    auto cbs = move(_callbacks);

    for (auto& cb : cbs) {
        ios.post([cb = move(cb)] {
                cb(asio::error::operation_aborted);
            });
    }
}
