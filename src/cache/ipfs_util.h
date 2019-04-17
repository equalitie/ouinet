#pragma once

#include "../util/signal.h"

namespace ouinet {

inline
std::string ipfs_cat( asio_ipfs::node& node
                    , boost::string_view cid
                    , Cancel& cancel
                    , asio::yield_context yield)
{
    std::function<void()> cancel_fn;
    auto cancel_handle = cancel.connect([&] { if (cancel_fn) cancel_fn(); });
    sys::error_code ec;
    std::string ret = node.cat(cid, cancel_fn, yield[ec]);
    if (cancel_handle) ec = asio::error::operation_aborted;
    return or_throw(yield, ec, std::move(ret));
}

// A lambda which captures the `node` by reference
// and gets the given `hash` from it.
#define IPFS_LOAD_FUNC(node) \
    [&](auto hash, auto& cancel, auto yield) { \
        return ipfs_cat(node, hash, cancel, yield); \
    }

} // namespace
