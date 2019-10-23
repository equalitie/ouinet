#pragma once

#include "../namespaces.h"
#include <set>
#include <vector>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/variant.hpp>

namespace ouinet { namespace util {

struct Network : public boost::variant< asio::ip::network_v4
                                      , asio::ip::network_v6 >
{
    using Parent = boost::variant< asio::ip::network_v4
                                 , asio::ip::network_v6 >;

    // Use Paren's constructors
    using Parent::Parent;

    bool is_v4() const {
        return boost::get<asio::ip::network_v4>(this);
    }

    bool has_address(asio::ip::address addr) const {
        struct visitor : public boost::static_visitor<bool> {
            const asio::ip::address& addr;

            visitor(const asio::ip::address& addr) : addr(addr) {}

            bool operator()(const asio::ip::network_v4& net) const {
                if (!addr.is_v4()) return false;
                auto range = net.hosts();
                return range.find(addr.to_v4()) != range.end();
            }

            bool operator()(const asio::ip::network_v6& net) const {
                if (!addr.is_v6()) return false;
                auto range = net.hosts();
                return range.find(addr.to_v6()) != range.end();
            }
        };

        return boost::apply_visitor(visitor(addr), *this);
    }
};

std::set<asio::ip::address> get_if_addrs(sys::error_code&);

std::vector<Network> get_networks(sys::error_code&);

}} // namespaces
