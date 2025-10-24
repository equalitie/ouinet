#pragma once

#include <functional>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system.hpp>

// Forward declarations for dns.rs.h
namespace ouinet::dns::bridge {
    class Completer;
}

#include "ouinet-rs/src/dns.rs.h"

namespace ouinet::dns {

/// A DNS resolver
class Resolver {
public:
    using Output = std::vector<boost::asio::ip::address>;

    Resolver();

    /// Resolve the given DNS name.
    Output resolve(const std::string& name, boost::asio::yield_context);

private:

    rust::Box<bridge::Resolver> _impl;
};

namespace bridge {

class Completer {
public:
    using Result = boost::system::result<ouinet::dns::Resolver::Output>;
    using Function = std::function<void(Result)>;

    explicit Completer(Function&& function);
    explicit Completer(const Function& function);

    void on_success(rust::Vec<IpAddress> addresses) const;
    void on_failure(rust::String error) const;

private:

    Function _function;
};

} // namespace bridge

} // namespace ouinet::dns
