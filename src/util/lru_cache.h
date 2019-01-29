#pragma once

#include <unordered_map>
#include <list>
#include <cstddef>
#include <stdexcept>

namespace ouinet { namespace util {

template<typename Key, typename Value>
class LruCache {
private:
    using KeyVal = std::pair<Key, Value>;
    using ListIter = typename std::list<KeyVal>::iterator;
    using Map = std::unordered_map<Key, ListIter>;
    using MapIter = typename Map::iterator;


public:
    class iterator {
        friend class LruCache;
        MapIter i;
    public:
        iterator(MapIter i) : i(i) {}
        KeyVal& operator*() { return *i->second; }
        KeyVal* operator->() { return &*i->second; }

        iterator& operator++() {
            ++i;
            return *this;
        }

        iterator operator++(int) {
            iterator ret{i};
            ++i;
            return ret;
        }

        bool operator==(iterator j) const {
            return i == j.i;
        }

        bool operator!=(iterator j) const {
            return i != j.i;
        }
    };

public:
    LruCache(size_t max_size)
        : _max_size(max_size) { }

    Value* put(const Key& key, Value value) {
        // When modifying this func be careful to handle the case
        // when `key` is a reference to the key already in the cache.
        // E.g. cache.put(i->key, "new value");

        auto it = _map.find(key);

        _list.push_front(KeyVal(key, std::move(value)));

        if (it != _map.end()) {
            _list.erase(it->second);
            it->second = _list.begin();
        }
        else {
            _map[key] = _list.begin();
        }

        if (_map.size() > _max_size) {
            auto last = _list.end();
            last--;
            _map.erase(last->first);
            _list.pop_back();
        }

        return &_list.begin()->second;
    }

    Value* get(const Key& key) {
        auto it = _map.find(key);

        if (it == _map.end()) return nullptr;

        _list.splice(_list.begin(), _list, it->second);

        assert(it->second == _list.begin());

        return &it->second->second;
    }

    bool exists(const Key& key) const {
        return _map.count(key) != 0;
    }

    size_t size() const {
        return _map.size();
    }

    bool empty() const { return _map.empty(); }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator begin() {
        return iterator{_map.begin()};
    }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator end() {
        return iterator{_map.end()};
    }

    iterator erase(iterator i) {
        auto j = std::next(i);
        _list->erase(i.i->second);
        _map->erase(i.i);
        return j;
    }

    void move_to_front(iterator i) {
        _list.splice(_list.begin(), _list, i.i->second);
    }

private:
    std::list<KeyVal> _list;
    std::unordered_map<Key, ListIter> _map;
    size_t _max_size;
};

}} // namespaces
