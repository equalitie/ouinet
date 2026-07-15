#pragma once

#include <list>
#include <cstddef>
#include <stdexcept>
#include <boost/filesystem.hpp>
#include <boost/asio/spawn.hpp>
#include <dirent.h>
#include <map>

#include "../namespaces.h"
#include "../defer.h"
#include "atomic_file.h"
#include "file_io.h"
#include "scheduler.h"
#include "bytes.h"
#include "async.h"

namespace ouinet {
    class Cancel;
}

namespace ouinet::util {

namespace persisten_lru_cache_detail {
    static const auto temp_file_prefix = "tmp.";
    static const auto temp_file_model = std::string(temp_file_prefix) + "%%%%-%%%%-%%%%-%%%%";

    uint64_t ms_since_epoch();
    fs::path path_from_key(const fs::path&, const std::string&);
    bool is_cache_entry(const struct dirent*, boost::filesystem::path&);
} // detail namespace

template<class Value>
class PersistentLruCache {
private:
    class Element;

    using Key = std::string;
    using KeyVal = std::pair<Key, std::shared_ptr<Element>>;

    using List = std::list<KeyVal>;
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

        const Value& value() const;
        const Key& key() const;

        [[nodiscard]]
        std::expected<async_file_handle, sys::error_code> open() const;
    };

private:
    /* private, use the static `load` function */
    PersistentLruCache( const AsioExecutor&
                      , boost::filesystem::path dir
                      , size_t max_size);

public:
    PersistentLruCache(const PersistentLruCache&) = delete;
    PersistentLruCache(PersistentLruCache&&) = delete;

    [[nodiscard]]
    static
    std::expected<std::unique_ptr<PersistentLruCache>, sys::error_code>
    load( boost::filesystem::path dir
        , size_t max_size
        , Async);

    [[nodiscard]]
    std::expected<void, sys::error_code>
    insert(std::string key, Value value, Async);

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

    ~PersistentLruCache();

private:
    fs::path path_from_key(const std::string&);

private:
    AsioExecutor _ex;
    boost::filesystem::path _dir;
    List _list;
    Map _map;
    size_t _max_size;
};

template<class Value>
class PersistentLruCache<Value>::Element {
public:
    [[nodiscard]]
    static
    std::expected<std::shared_ptr<Element>, sys::error_code>
    read(fs::path path, uint64_t* ts_out, Async yield)
    {
        sys::error_code ec;

        auto on_exit = defer([&] { if (ec) std::ignore = file_io::remove_file(path); });

        auto file = file_io::open_readonly(yield.get_executor(), path);
        if (!file) return std::unexpected(file.error());

        auto ts = file_io::read_number<uint64_t>(*file, yield);
        if (!ts) return std::unexpected(ts.error());

        if (ts_out) *ts_out = *ts;

        auto key_size = file_io::read_number<uint32_t>(*file, yield);
        if (!key_size) return std::unexpected(key_size.error());

        std::string key(*key_size, '\0');
        auto read = file_io::read(*file, asio::buffer(key), yield);
        if (!read) return std::unexpected(read.error());

        Value value;

        auto vr = value.read(*file, yield);
        if (!vr) return std::unexpected(vr.error());

        return std::make_shared<Element>( yield.get_executor()
                                        , std::move(key)
                                        , std::move(path)
                                        , std::move(value));
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    update(Value value, Async yield)
    {
        using namespace persisten_lru_cache_detail;

        _value = std::move(value);

        auto ts = ms_since_epoch();

        sys::error_code ec;

        // Create a new entry file "atomically" (at least inside the program)
        // by writing data to a temporary file and replacing the existing file.
        // Otherwise we might be overwriting old data that others are reading.
        auto af = atomic_file::make( _ex, _path, persisten_lru_cache_detail::temp_file_model);
        if (!af) return std::unexpected(af.error());
        auto& f = af->lowest_layer();
        if (auto r = file_io::write_number<uint64_t>(f, ts, yield); !r) {
            return std::unexpected(r.error());
        }

        if (auto r = file_io::write_number<uint32_t>(f, _key.size(), yield); !r) {
            return std::unexpected(r.error());
        }

        if (auto r = file_io::write(f, asio::buffer(_key), yield); !r) {
            return std::unexpected(r.error());
        }

        //if (auto r = file_io::write(f, asio::buffer(value), cancel, yield[ec]); !r) {
        //    return std::unexpected(r.error());
        //}
        if (auto r = _value.write(f, yield); !r) {
            return std::unexpected(r.error());
        }

        af->commit(ec);
        if (ec) return std::unexpected(ec);

        return {};
    }

    const Value& value() const {
        return _value;
    }

    // Read-only byte-oriented access to on-disk data.
    [[nodiscard]]
    std::expected<async_file_handle, sys::error_code> open_value() const {
        auto f = file_io::open_readonly(_ex, _path);
        if (!f) return std::unexpected(f.error());
        auto r = file_io::fseek(*f, content_start());
        if (!r) return std::unexpected(r.error());
        return std::move(*f);
    }

    ~Element()
    {
        if (!_keep_file_on_destruct) {
            std::ignore = file_io::remove_file(_path);
        }
    }

    Element( const AsioExecutor& ex
           , std::string key
           , fs::path path
           , Value value)
        : _ex(ex)
        , _scheduler(ex, 1)
        , _key(std::move(key))
        , _path(std::move(path))
        , _value(std::move(value))
    {}

    void keep_file_on_destruct() {
        _keep_file_on_destruct = true;
    }

    [[nodiscard]]
    std::expected<Scheduler::Slot, sys::error_code> lock(Async yield)
    {
        return _scheduler.wait_for_slot(yield);
    }

    const std::string& key() const { return _key; }

private:
    size_t content_start() const {
        return sizeof(uint64_t) // time stamp
             + sizeof(uint32_t) // key size
             + _key.size();
    }

private:
    AsioExecutor _ex;
    Scheduler _scheduler;
    std::string _key;
    fs::path _path;
    Value _value;
    bool _keep_file_on_destruct = false;
};

template<class Value>
[[nodiscard]]
inline
std::expected<std::unique_ptr<PersistentLruCache<Value>>, sys::error_code>
PersistentLruCache<Value>::load( boost::filesystem::path dir
                               , size_t max_size
                               , Async yield)
{
    using namespace persisten_lru_cache_detail;

    using Ret = std::unique_ptr<PersistentLruCache<Value>>;

    if (!dir.is_absolute()) {
        dir = fs::absolute(dir);
    }

    if (auto r = file_io::check_or_create_directory(dir); !r) {
        return std::unexpected(r.error());
    }

    Ret lru(new PersistentLruCache<Value>(yield.get_executor(), dir, max_size));

    // Id helps us resolve the case when two entries have the same timestamp
    using Id = std::pair<uint64_t, uint64_t>;

    std::map<Id, std::shared_ptr<Element>> elements;

    {
        DIR* directory = opendir(dir.string().c_str());
        auto close_dir = defer([&] { if (directory != nullptr) closedir(directory); });

        uint64_t i = 0;
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            if (is_cache_entry(entry, dir)) {
                fs::path path(dir / entry->d_name);
                uint64_t ts;
                auto e = Element::read(path, &ts, yield);

                if (!e) continue;

                elements.insert({Id{ts, i++}, std::move(*e)});
            }
        }
    }

    while (elements.size() > max_size) {
        auto i = elements.begin();
        elements.erase(i);
    }

    for (auto p : elements) {
        auto e = p.second;

        auto map_i = lru->_map.find(e->key());
        assert(map_i == lru->_map.end());
        lru->_list.push_front({e->key(), e});
        lru->_map[e->key()] = lru->_list.begin();
    }

    return lru;
}

template<class Value>
inline
PersistentLruCache<Value>::PersistentLruCache( const AsioExecutor& ex
                                             , boost::filesystem::path dir
                                             , size_t max_size)
    : _ex(ex)
    , _dir(std::move(dir))
    , _max_size(max_size)
{
}

template<class Value>
inline
std::expected<void, sys::error_code>
PersistentLruCache<Value>::insert(std::string key, Value value, Async yield)
{
    auto it = _map.find(key);

    std::shared_ptr<Element> e;

    if (it != _map.end()) {
        e = it->second->second;
    } else {
        // TODO: Value is set twice, here and at the end of this fn
        e = std::make_shared<Element>(_ex, key, path_from_key(key), value);
    }

    _list.push_front({key, e});

    if (it != _map.end()) {
        _list.erase(it->second);
        it->second = _list.begin();
    }
    else {
        it = _map.insert({key, _list.begin()}).first;
    }

    if (_map.size() > _max_size) {
        auto last = prev(_list.end());
        if (last->first == it->first) e = nullptr;
        _map.erase(last->first);
        _list.pop_back();
    }

    if (!e) return {};

    auto slot = e->lock(yield);
    if (!slot) return std::unexpected(slot.error());
    return e->update(std::move(value), yield);
}

template<class Value>
inline
typename PersistentLruCache<Value>::iterator
PersistentLruCache<Value>::find(const std::string& key)
{
    auto it = _map.find(key);

    if (it == _map.end()) return it;

    // Move it to the front
    auto list_it = it->second;
    _list.splice(_list.begin(), _list, list_it);
    assert(list_it == _list.begin());

    return it;
}

template<class Value>
inline
fs::path PersistentLruCache<Value>::path_from_key(const std::string& key)
{
    return persisten_lru_cache_detail::path_from_key(_dir, key);
}

template<class Value>
inline
const Value&
PersistentLruCache<Value>::iterator::value() const
{
    return i->second->second->value();
}

template<class Value>
inline
const typename PersistentLruCache<Value>::Key&
PersistentLruCache<Value>::iterator::key() const
{
    return i->first;
}

template<class Value>
[[nodiscard]]
inline
std::expected<async_file_handle, sys::error_code>
PersistentLruCache<Value>::iterator::open() const
{
    return i->second->second->open_value();
}

template<class Value>
inline
PersistentLruCache<Value>::~PersistentLruCache()
{
    for (auto& kv : _list) {
        kv.second->keep_file_on_destruct();
    }
}

} // namespaces
