#pragma once

#include <functional>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system.hpp>

// Forward declarations for dns.rs.h
namespace ouinet::dns::bridge {
    class BasicCompleter;
}

#include "ouinet-rs/src/dns.rs.h"

namespace ouinet::dns {

using bridge::Error;

/// A DNS resolver
class Resolver {
public:
    using Output = std::vector<boost::asio::ip::address>;

    Resolver();

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    Resolver(Resolver&&) = default;
    Resolver& operator=(Resolver&&) = default;

    /// Resolve the given DNS name.
    Output resolve(const std::string& name, boost::asio::yield_context);

    /// Close this DNS resolver, cancelling any ongoing lookups. Any subsequent lookups return with
    /// a `NotFound` error.
    void close();

private:

    std::optional<rust::Box<bridge::Resolver>> _impl;
};

/// A category of DNS errors
class ErrorCategory : public boost::system::error_category {
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

extern ErrorCategory error_category;

namespace bridge {

class BasicCompleter {
public:
    explicit BasicCompleter(boost::asio::cancellation_slot&&);

    virtual void complete(Error error_code, rust::Vec<IpAddress> addresses) = 0;

    void on_cancel(rust::Box<CancellationToken>);

private:
    boost::asio::cancellation_slot _cancellation_slot;
};

inline boost::system::error_code make_error_code(Error error) noexcept {
    if (error == Error::Cancelled) {
        return boost::asio::error::operation_aborted;
    } else {
        return boost::system::error_code(static_cast<int>(error), error_category);
    }
}

} // namespace bridge

} // namespace ouinet::dns

namespace boost::system {
template<>
struct is_error_code_enum<ouinet::dns::bridge::Error> {
    static const bool value = true;
};
}