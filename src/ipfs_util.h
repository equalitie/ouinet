#pragma once

#define IPFS_LOAD_FUNC(node) \
    [&](auto hash, auto& cancel, auto yield) { \
        function<void()> cancel_fn; \
        auto cancel_handle = cancel.connect([&] { if (cancel_fn) cancel_fn(); }); \
        return (node).cat(hash, cancel_fn, yield); \
    }
