#pragma once

namespace ouinet { namespace bittorrent {

namespace ProximityMapDetail {
    struct Cmp {
        NodeID pivot;
    
        bool operator()(const NodeID& l, const NodeID& r) const {
            return pivot.closer_to(l, r);
        }
    };
}

template<class Value>
class ProximityMap : private std::map<NodeID, Value, ProximityMapDetail::Cmp> {
private:
    using Cmp = ProximityMapDetail::Cmp;
    using Parent = std::map<NodeID, Value, Cmp>;

public:
    ProximityMap(const NodeID& pivot, size_t max_size)
        : Parent(Cmp{pivot})
        , _max_size(max_size)
    {}

    using Parent::begin;
    using Parent::end;
    using Parent::size;
    using Parent::iterator;
    using Parent::const_iterator;
    using Parent::erase;

    bool full() const {
        return Parent::size() >= _max_size;
    }

    // TODO: Return value as in Parent::insert
    void insert(typename Parent::value_type v) {
        if (_max_size == 0) return;

        if (!full()) {
            Parent::insert(std::move(v));
            return;
        }

        auto last = std::prev(Parent::end());

        if (Parent::key_comp()(last->first, v.first)) {
            return;
        }

        Parent::erase(last);
        Parent::insert(std::move(v));
    }

    bool would_insert(const NodeID& id) const {
        if (_max_size == 0) return false;
        if (Parent::size() < _max_size) return true;
        auto last = std::prev(Parent::end());
        return Parent::key_comp()(id, last->first);
    }

private:
    size_t _max_size;
};


}} // namespaces
