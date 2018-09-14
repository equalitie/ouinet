#pragma once

#include <string>
#include <boost/asio/spawn.hpp>
#include <boost/optional.hpp>
#include "../namespaces.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

class Resolver {
private:
    struct Loop;
    using OnResolve = std::function<void(std::string, asio::yield_context)>;

public:
    Resolver( asio_ipfs::node&
            , const std::string& key
            , const boost::optional<util::Ed25519PublicKey>&
            , OnResolve);
    Resolver(const Resolver&) = delete;

    ~Resolver();

private:
    std::shared_ptr<Loop> _ipfs_loop;
};

} // namespace
