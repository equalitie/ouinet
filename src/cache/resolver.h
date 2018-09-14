#pragma once

#include <string>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/optional.hpp>
#include "../namespaces.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace util { class Ed25519PublicKey; }}
namespace ouinet { namespace bittorrent { class MainlineDht; }}

namespace ouinet {

class Resolver {
private:
    struct Loop;
    using OnResolve = std::function<void(std::string, asio::yield_context)>;

public:
    Resolver( asio_ipfs::node&
            , const std::string& key
            , bittorrent::MainlineDht& bt_dht
            , const boost::optional<util::Ed25519PublicKey>&
            , OnResolve);

    Resolver(const Resolver&) = delete;

    ~Resolver();

private:
    asio::io_service& _ios;
    std::shared_ptr<Loop> _ipfs_loop;
    std::shared_ptr<Loop> _bt_loop;
};

} // namespace
