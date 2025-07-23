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
    : _impl({ bridge::new_client(
#ifdef _WIN32
                  rust::String(repo_root_path.string())
#else
                  rust::String(repo_root_path.native())
#endif
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

Request Client::new_cache_in_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_cache_in_request()};
}

Request Client::new_cache_out_request() {
    if (!_impl) return Request{{}};
    return Request{(*_impl)->new_cache_out_request()};
}

std::optional<std::string> Client::current_device_id() const {
    if (!_impl) return {};
    rust::String str = (*_impl)->current_device_id();
    return std::string(str.data(), str.size());
}

std::optional<std::string> Client::current_record_id() const {
    if (!_impl) return {};
    rust::String str = (*_impl)->current_record_id();
    return std::string(str.data(), str.size());
}

void Client::bridge_transfer_i2c(size_t byte_count) {
    if (!_impl) return;
    (*_impl)->bridge_transfer_i2c(byte_count);
}

void Client::bridge_transfer_c2i(size_t byte_count) {
    if (!_impl) return;
    (*_impl)->bridge_transfer_c2i(byte_count);
}

SetAuxResult Client::set_aux_key_value(std::string_view record_id, std::string_view key, std::string_view value) {
    if (!_impl) return SetAuxResult::Noop;

    bool ok = (*_impl)->set_aux_key_value(
            rust::String(record_id.data(), record_id.size()),
            rust::String(key.data(), key.size()),
            rust::String(value.data(), value.size()));

    return ok ? SetAuxResult::Ok : SetAuxResult::BadRecordId;
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

void Request::increment_transfer_size(size_t added) {
    if (!_impl) return;
    (*_impl)->increment_transfer_size(added);
}

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
