#include <iostream>
#include <sstream>
#include "metrics.h"
#include "record_processor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

using namespace std;
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

void Client::set_processor(util::AsioExecutor executor, AsyncCallback record_processor) {
    _impl->set_processor(
            make_unique<bridge::CxxRecordProcessor>(
                move(executor),
                move(record_processor)));
}

MainlineDht Client::mainline_dht()
{
    return MainlineDht{_impl->new_mainline_dht()};
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

} // namespace
