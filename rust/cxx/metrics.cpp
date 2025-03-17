#include <iostream>
#include "metrics.h"
#include "record_processor.h"
#include "util/executor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

namespace asio = boost::asio;

//--------------------------------------------------------------------

Client Client::noop() {
    return Client();
}

Client::Client(const fs::path& repo_root_path, EncryptionKey encryption_key)
    : _impl({
            bridge::new_client( rust::String(repo_root_path.native())
                              , std::move(*encryption_key._impl))})
{
}

void Client::enable(util::AsioExecutor executor, AsyncCallback record_processor) {
    if (!_impl) return;

    (*_impl)->set_processor(
            make_unique<bridge::CxxRecordProcessor>(
                std::move(executor),
                std::move(record_processor)));
    _is_enabled = true;
}

void Client::disable() {
    if (!_impl) return;
    (*_impl)->set_processor(nullptr);
    _is_enabled = false;
}

MainlineDht Client::mainline_dht()
{
    if (!_impl) return MainlineDht{{}};
    return MainlineDht{(*_impl)->new_mainline_dht()};
}

Request Client::new_origin_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_origin_request()};
}

Request Client::new_private_injector_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_private_injector_request()};
}

Request Client::new_public_injector_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_public_injector_request()};
}

Request Client::new_cache_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_cache_request()};
}

//--------------------------------------------------------------------

DhtNode MainlineDht::dht_node_ipv4() {
    if (!_impl) return DhtNode{{}};
    return DhtNode{(*_impl)->new_dht_node(true)};
}

DhtNode MainlineDht::dht_node_ipv6() {
    if (!_impl) return DhtNode{{}};
    return DhtNode{(*_impl)->new_dht_node(false)};
}

//--------------------------------------------------------------------

Bootstrap DhtNode::bootstrap() {
    if (!_impl) return Bootstrap{{}};
    return Bootstrap{(*_impl)->new_bootstrap()};
}

//--------------------------------------------------------------------

void Bootstrap::mark_success() {
    if (!_impl) return;
    (*_impl)->mark_success();
}

//--------------------------------------------------------------------

void Request::finish(boost::system::error_code ec) {
    if (!_impl) return;

    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            (*_impl)->mark_failure();
        }
    } else {
        (*_impl)->mark_success();
    }
}

//--------------------------------------------------------------------

std::optional<EncryptionKey> EncryptionKey::validate(const std::string& key_str) {
    try {
        return EncryptionKey(bridge::validate_encryption_key(rust::String(key_str)));
    } catch (...) {
        return {};
    }
}

} // namespace
