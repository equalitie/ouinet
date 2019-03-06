#pragma once

#include <unordered_map>
#include <list>
#include <cstddef>
#include <stdexcept>
#include <boost/filesystem.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "../namespaces.h"
#include "signal.h"

namespace ouinet { namespace util {

class PersistentLruCache {
private:
    struct Element;

    using Key = std::string;
    using KeyVal = std::pair<Key, std::shared_ptr<Element>>;

    using List = typename std::list<KeyVal>;
    using ListIter = typename List::iterator;

    using Map = std::map<Key, ListIter>;
    using MapIter = typename Map::iterator;

public:
    class iterator {
        friend class PersistentLruCache;
        MapIter i;
    public:
        iterator(MapIter i) : i(i) {}

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

        std::string value(Cancel&, asio::yield_context);
    };

private:
    /* private, use the static `load` function */
    PersistentLruCache( asio::io_service&
                      , boost::filesystem::path dir
                      , size_t max_size);

public:
    PersistentLruCache(const PersistentLruCache&) = delete;
    PersistentLruCache(PersistentLruCache&&) = delete;

    static
    std::unique_ptr<PersistentLruCache> load( asio::io_service&
                                            , boost::filesystem::path dir
                                            , size_t max_size
                                            , Cancel&
                                            , asio::yield_context);

    void insert( std::string key
               , std::string value
               , Cancel&
               , asio::yield_context);

    iterator find(const std::string& key);

    bool exists(const std::string& key) const {
        return _map.count(key) != 0;
    }

    size_t size() const {
        return _map.size();
    }

    bool empty() const { return _map.empty(); }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator begin() {
        return iterator(_map.begin());
    }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator end() {
        return iterator(_map.end());
    }

    void move_to_front(iterator i) {
        _list.splice(_list.begin(), _list, i.i->second);
    }

    const boost::filesystem::path& dir() const {
        return _dir;
    }

private:
    fs::path path_from_key(const std::string&);

private:
    asio::io_service& _ios;
    boost::filesystem::path _dir;
    List _list;
    Map _map;
    size_t _max_size;
};

}} // namespaces
