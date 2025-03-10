#include <iostream>
#include <sstream>
#include "metrics.h"
#include "record_processor.h"
#include "util/executor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

namespace asio = boost::asio;

//--------------------------------------------------------------------

Client::Client()
    : _impl(bridge::new_noop_client())
{
}

Client::Client(fs::path repo_root_path)
    : _impl(bridge::new_client(rust::String(repo_root_path.native())))
{
}

void Client::enable(util::AsioExecutor executor, AsyncCallback record_processor) {
    _impl->set_processor(
            make_unique<bridge::CxxRecordProcessor>(
                std::move(executor),
                std::move(record_processor)));
    _is_enabled = true;
}

void Client::disable() {
    _impl->set_processor(nullptr);
    _is_enabled = false;
}

MainlineDht Client::mainline_dht()
{
    return MainlineDht{_impl->new_mainline_dht()};
}

Request Client::new_origin_request() {
    return Request{_impl->new_origin_request()};
}

Request Client::new_injector_request() {
    return Request{_impl->new_injector_request()};
}

Request Client::new_cache_request() {
    return Request{_impl->new_cache_request()};
}

//--------------------------------------------------------------------

DhtNode MainlineDht::dht_node_ipv4() {
    return DhtNode{_impl->new_dht_node(true)};
}

DhtNode MainlineDht::dht_node_ipv6() {
    return DhtNode{_impl->new_dht_node(false)};
}

//--------------------------------------------------------------------

Bootstrap DhtNode::bootstrap() {
    return Bootstrap{_impl->new_bootstrap()};
}

//--------------------------------------------------------------------

void Bootstrap::mark_success(asio::ip::udp::endpoint wan_endpoint) {
    std::stringstream ss;
    ss << wan_endpoint;
    _impl->mark_success(rust::String(ss.str()));
}

//--------------------------------------------------------------------

void Request::start() {
    _impl->mark_started();
}

void Request::success() {
    _impl->mark_success();
}

void Request::failure() {
    _impl->mark_failure();
}

} // namespace
