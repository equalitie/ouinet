#pragma once

#include <boost/optional.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <memory>
#include <map>
#include <iostream>
#include "../namespaces.h"
#include "../defer.h"
#include "../util/signal.h"

namespace ouinet {

class BTree {
public:
    using Key   = std::string;
    using Value = std::string;
    using Hash  = std::string;

    using CatOp    = std::function<Value(const Hash&,  Cancel&, asio::yield_context)>;
    using AddOp    = std::function<Hash (const Value&, asio::yield_context)>;
    using RemoveOp = std::function<void (const Hash&,  asio::yield_context)>;

    struct Node; // public, but opaque

public:
    // Note: due to lazy-async nature of the nodes of this tree
    // we can't use the standard std::iterator-like interface.
    class Iterator {
    private:
        friend class BTree;
        struct Impl;

    public:
        Key key() const;
        Value value() const;

        void advance(Cancel&, asio::yield_context);
        bool is_end() const;

    private:
        Iterator(std::shared_ptr<Impl>);

    private:
        std::shared_ptr<Impl> _impl;
    };

public:
    BTree( CatOp    = nullptr
         , AddOp    = nullptr
         , RemoveOp = nullptr
         , size_t max_node_size = 512);

    Value find(const Key&, Cancel&, asio::yield_context);

    void insert(Key, Value, asio::yield_context);

    bool check_invariants() const;

    std::string root_hash() const {
        if (!_root) return {};
        return _root->hash;
    }

    void load(Hash, asio::yield_context);

    ~BTree();

    void debug(bool v) { _debug = v; }

    size_t local_node_count() const;

    Iterator begin(Cancel&, asio::yield_context) const;

private:
    void raw_insert(Key, Value, asio::yield_context);

    Value lazy_find( const Hash&
                   , std::unique_ptr<Node>&
                   , const Key&
                   , const CatOp&
                   , Cancel&
                   , asio::yield_context) const;

    void try_remove(Hash&, asio::yield_context) const;

private:
    size_t _max_node_size;

    struct Root {
        std::unique_ptr<Node> node;
        std::string hash;
    };

    std::shared_ptr<Root> _root;

    std::map<Key, Value> _insert_buffer;
    bool _is_inserting = false;

    CatOp _cat_op;
    AddOp _add_op;
    RemoveOp _remove_op;

    std::shared_ptr<bool> _was_destroyed;

    bool _debug = false;
};


} // namespace
