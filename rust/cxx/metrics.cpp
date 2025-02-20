#include <iostream>
#include <sstream>
#include "metrics.h"
#include "record_processor.h"
#include "util/executor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

using namespace std;
namespace asio = boost::asio;

//--------------------------------------------------------------------

Client::Client( util::AsioExecutor executor
              , fs::path repo_root_path
              , AsyncCallback process_report)
    : _impl(bridge::new_client
                ( rust::String(repo_root_path.native())
                , make_unique<bridge::CxxRecordProcessor>(move(executor), move(process_report))))
{
}

MainlineDht Client::mainline_dht()
{
    return MainlineDht{_impl->new_mainline_dht()};
}

//--------------------------------------------------------------------

DhtNode MainlineDht::dht_node_ipv4() {
    return DhtNode{_impl->new_dht_node("ipv4")};
}

DhtNode MainlineDht::dht_node_ipv6() {
    return DhtNode{_impl->new_dht_node("ipv6")};
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
