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
    using Result = boost::system::result<Output>;

    Resolver();

    /// Resolve the given DNS name.
    Result resolve(const std::string& name, boost::asio::yield_context yield) {
        // TODO:
        //
        // _impl->resolve(name, std::make_unique<Completer>([](auto result) {
        //     // ...
        // }));
    }

private:

    rust::Box<bridge::Resolver> _impl;
};

namespace bridge {

class Completer {
public:
    using Function = std::function<void(ouinet::dns::Resolver::Result)>;

    explicit Completer(Function&& function);
    explicit Completer(const Function& function);

    void on_success(rust::Vec<IpAddress> addresses) const;
    void on_failure(rust::String error) const;

private:

    Function _function;
};

} // namespace bridge

} // namespace ouinet::dns
