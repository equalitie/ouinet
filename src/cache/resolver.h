#pragma once

#include <string>
#include <boost/asio/spawn.hpp>
#include "../namespaces.h"

namespace asio_ipfs { class node; }

namespace ouinet {

class Resolver {
private:
    struct Loop;
    using OnResolve = std::function<void(std::string, asio::yield_context)>;

public:
    Resolver(asio_ipfs::node&, const std::string& key, OnResolve);
    Resolver(const Resolver&) = delete;

    ~Resolver();

private:
    std::shared_ptr<Loop> _loop;
};

} // namespace
