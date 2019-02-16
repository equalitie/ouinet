#include "persistent_lru_cache.h"
#include "scheduler.h"
#include "../namespaces.h"
#include "../or_throw.h"
#include "../defer.h"
#include "bytes.h"
#include "sha1.h"

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <iostream>

using namespace std;
using namespace ouinet;
using namespace ouinet::util;
using boost::string_view;

#if !BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
#error "OS does not have a support for POSIX stream descriptors"
#endif

// https://www.boost.org/doc/libs/1_69_0/libs/system/doc/html/system.html#ref_boostsystemerror_code_hpp
namespace errc = boost::system::errc;
namespace posix = asio::posix;

// TODO: In order to keep memory requirements low, it would be better
// to return a new kind of file stream that users would be required to
// lock before read/write operations. Then return that instead of
// raw memory chunks.

static void create_or_check_directory(const fs::path& dir, sys::error_code& ec)
{
    if (exists(dir)) {
        if (!is_directory(dir)) {
            ec = make_error_code(errc::not_a_directory);
            return;
        }

        // TODO: Check if we can read/write
    } else {
        if (!create_directories(dir, ec)) {
            if (!ec) ec = make_error_code(errc::operation_not_permitted);
            return;
        }
        assert(is_directory(dir));
    }
}

static uint64_t ms_since_epoch()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    return duration_cast<milliseconds>(now.time_since_epoch()).count();
}

static
sys::error_code last_error()
{
    return make_error_code(static_cast<errc::errc_t>(errno));
}

static void fseek(posix::stream_descriptor& f, size_t pos, sys::error_code& ec)
{
    if (lseek(f.native_handle(), pos, SEEK_SET) == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
    }
}

static
posix::stream_descriptor open( asio::io_service& ios
                             , const fs::path& p
                             , sys::error_code& ec)
{
    int file = open(p.c_str(), O_RDWR | O_CREAT , S_IRUSR | S_IWUSR);

    if (file == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return asio::posix::stream_descriptor(ios);
    }

    asio::posix::stream_descriptor f(ios, file);
    fseek(f, 0, ec);

    return f;
}

static
void truncate( posix::stream_descriptor& f
             , size_t new_length
             , sys::error_code& ec)
{
    if (ftruncate(f.native_handle(), new_length) != 0) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
    }
}

static
void read_data( posix::stream_descriptor& f
              , asio::mutable_buffer b
              , Cancel& cancel
              , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_read(f, b, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

static
void write_data( posix::stream_descriptor& f
               , asio::const_buffer b
               , Cancel& cancel
               , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_write(f, b, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

template<typename T>
static
T read_number( posix::stream_descriptor& f
             , Cancel& cancel
             , asio::yield_context yield)
{
    T num;
    sys::error_code ec;
    // TODO: endianness? (also for writing)
    read_data(f, asio::buffer(&num, sizeof(num)), cancel, yield[ec]);
    return or_throw<T>(yield, ec, move(num));
}

template<typename T>
static
void write_number( posix::stream_descriptor& f
                 , T num
                 , Cancel& cancel
                 , asio::yield_context yield)
{
    sys::error_code ec;
    // TODO: endianness? (also for reading)
    write_data(f, asio::buffer(&num, sizeof(num)), cancel, yield[ec]);
    return or_throw(yield, ec);
}

static
void remove_file(const fs::path& p)
{
    if (!exists(p)) return;
    assert(is_regular_file(p));
    if (!is_regular_file(p)) return;
    sys::error_code ignored_ec;
    fs::remove(p, ignored_ec);
}

class PersistentLruCache::Element {
public:
    static
    shared_ptr<Element> open( asio::io_service& ios
                            , fs::path path
                            , uint64_t* ts_out
                            , Cancel& cancel
                            , asio::yield_context yield)
    {
        using Ret = shared_ptr<Element>;

        sys::error_code ec;

        auto on_exit = defer([&] { if (ec) remove_file(path); });

        auto file = ::open(ios, path, ec);
        if (ec) return or_throw<Ret>(yield, ec);

        auto ts = read_number<uint64_t>(file, cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        if (ts_out) *ts_out = ts;

        auto key_size = read_number<uint32_t>(file, cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        string key(key_size, '\0');
        read_data(file, asio::buffer(key), cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        return make_shared<Element>(ios, move(key), move(path));
    }

    void update( vector<uint8_t> value
               , Cancel& cancel
               , asio::yield_context yield)
    {
        auto ts = ms_since_epoch();

        sys::error_code ec;

        auto f = ::open(_ios, _path, ec);
        if (!ec) truncate(f, content_start() + value.size(), ec);
        if (!ec) fseek(f, 0, ec);
        if (!ec) write_number<uint64_t>(f, ts, cancel, yield[ec]);
        if (!ec) write_number<uint32_t>(f, _key.size(), cancel, yield[ec]);
        if (!ec) write_data(f, asio::buffer(_key), cancel, yield[ec]);
        if (!ec) write_data(f, asio::buffer(value), cancel, yield[ec]);

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec);
    }

    void update(Cancel& cancel, asio::yield_context yield)
    {
        auto ts = ms_since_epoch();

        sys::error_code ec;

        auto f = ::open(_ios, _path, ec);
        if (!ec) fseek(f, 0, ec);
        if (!ec) write_number<uint64_t>(f, ts, cancel, yield[ec]);

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec);
    }

    vector<uint8_t> value(Cancel& cancel, asio::yield_context yield)
    {
        auto ts = ms_since_epoch();

        sys::error_code ec;

        size_t f_size = fs::file_size(_path, ec);
        vector<uint8_t> ret;

        auto f = ::open(_ios, _path, ec);
        if (ec) goto finish;

        fseek(f, 0, ec);
        if (ec) goto finish;

        write_number<uint64_t>(f, ts, cancel, yield[ec]);
        if (ec) goto finish;

        fseek(f, content_start(), ec);
        if (ec) goto finish;

        ret.resize(f_size - content_start());
        read_data(f, asio::buffer(ret), cancel, yield[ec]);

        finish:

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec, move(ret));
    }

    ~Element()
    {
        if (_remove_on_destruct) remove_file(_path);
    }

    Element( asio::io_service& ios
           , string key
           , fs::path path)
        : _ios(ios)
        , _scheduler(ios, 1)
        , _key(move(key))
        , _path(move(path))
    {}

    void remove_file_on_destruct() {
        _remove_on_destruct = true;
    }

    Scheduler::Slot lock(Cancel& cancel, asio::yield_context yield)
    {
        return _scheduler.wait_for_slot(cancel, yield);
    }

    const string& key() const { return _key; }

private:
    size_t content_start() const {
        return sizeof(uint64_t) // time stamp
             + sizeof(uint32_t) // key size
             + _key.size();
    }

private:
    asio::io_service& _ios;
    Scheduler _scheduler;
    string _key;
    fs::path _path;
    bool _remove_on_destruct = false;
};

/* static */
unique_ptr<PersistentLruCache>
PersistentLruCache::load( asio::io_service& ios
                        , boost::filesystem::path dir
                        , size_t max_size
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    using Ret = unique_ptr<PersistentLruCache>;

    sys::error_code ec;

    if (!dir.is_absolute()) {
        dir = fs::absolute(dir);
    }

    if (!ec) create_or_check_directory(dir, ec);

    if (ec) {
        cerr << "PersistentLruCache cannot use diretory \""
             << dir << "\"" << endl;
        return or_throw<Ret>(yield, ec);
    }

    unique_ptr<PersistentLruCache> lru(new PersistentLruCache(ios, dir, max_size));

    // Id helps us resolve the case when two entries have the same timestamp
    using Id = std::pair<uint64_t, uint64_t>;

    std::map<Id, shared_ptr<Element>> elements;

    uint64_t i = 0;
    for (auto file : fs::directory_iterator(dir)) {
        uint64_t ts;
        auto e = Element::open(ios, file, &ts, cancel, yield[ec]);

        if (cancel) {
            return or_throw<Ret>(yield, asio::error::operation_aborted);
        }

        if (ec) continue;

        elements.insert({Id{ts, i++}, e});
    }

    while (elements.size() > max_size) {
        auto i = elements.begin();
        i->second->remove_file_on_destruct();
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

PersistentLruCache::PersistentLruCache( asio::io_service& ios
                                      , boost::filesystem::path dir
                                      , size_t max_size)
    : _ios(ios)
    , _dir(move(dir))
    , _max_size(max_size)
{
}

void PersistentLruCache::insert( string key
                               , std::vector<uint8_t> value
                               , Cancel& cancel
                               , asio::yield_context yield)
{
    auto it = _map.find(key);

    shared_ptr<Element> e;

    if (it == _map.end()) {
        e = make_shared<Element>(_ios, key, path_from_key(key));
    } else {
        e = move(it->second->second);
    }

    _list.push_front({key, e});

    if (it != _map.end()) {
        it->second->second->remove_file_on_destruct();
        _list.erase(it->second);
        it->second = _list.begin();
    }
    else {
        it = _map.insert({key, _list.begin()}).first;
    }

    if (_map.size() > _max_size) {
        auto last = prev(_list.end());
        if (last->first == it->first) e = nullptr;
        last->second->remove_file_on_destruct();
        _map.erase(last->first);
        _list.pop_back();
    }

    if (!e) return;

    sys::error_code ec;
    auto slot = e->lock(cancel, yield[ec]);
    if (ec) return or_throw(yield, ec);
    e->update(move(value), cancel, yield);
}

PersistentLruCache::iterator PersistentLruCache::find(const string& key)
{
    auto it = _map.find(key);

    if (it == _map.end()) return it;

    auto list_it = it->second;

    _list.splice(_list.begin(), _list, list_it);

    assert(list_it == _list.begin());

    return it;
}

fs::path PersistentLruCache::path_from_key(const std::string& key)
{
    return _dir / bytes::to_hex(sha1(key));
}

vector<uint8_t>
PersistentLruCache::iterator::value(Cancel& cancel, asio::yield_context yield)
{
    // Capture shared_ptr here to make sure element doesn't
    // get deleted while reading from the file.
    shared_ptr<Element> e = i->second->second;

    sys::error_code ec;

    auto lock = e->lock(cancel, yield[ec]);
    if (ec) return or_throw<vector<uint8_t>>(yield, ec);

    return e->value(cancel, yield);
}
