#include "local_client.h"

#include "../async_sleep.h"
#include "http_store.h"


#define _LOGPFX "cache/local: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)
#define _YDEBUG(y, ...) do { if (logger.get_threshold() <= DEBUG) y.log(DEBUG, __VA_ARGS__); } while (false)
#define _YERROR(y, ...) do { if (logger.get_threshold() <= ERROR) y.log(ERROR, __VA_ARGS__); } while (false)


using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

namespace fs = boost::filesystem;


struct GarbageCollector {
    cache::HttpStore& http_store;  // for looping over entries
    cache::HttpStore::keep_func keep;  // caller-provided checks

    asio::executor _executor;
    Cancel _cancel;

    GarbageCollector( cache::HttpStore& http_store
                    , cache::HttpStore::keep_func keep
                    , asio::executor ex)
        : http_store(http_store)
        , keep(move(keep))
        , _executor(ex)
    {}

    ~GarbageCollector() { _cancel(); }

    void start()
    {
        asio::spawn(_executor, [&] (asio::yield_context yield) {
            TRACK_HANDLER();
            Cancel cancel(_cancel);

            _DEBUG("Garbage collector started");
            while (!cancel) {
                sys::error_code ec;
                async_sleep(_executor, chrono::minutes(7), cancel, yield[ec]);
                if (cancel || ec) break;

                _DEBUG("Collecting garbage...");
                http_store.for_each([&] (auto rr, auto y) {
                    sys::error_code e;
                    auto k = keep(std::move(rr), y[e]);
                    if (cancel) ec = asio::error::operation_aborted;
                    return or_throw(y, e, k);
                }, cancel, yield[ec]);
                if (ec) _WARN("Collecting garbage: failed;"
                              " ec=", ec);
                _DEBUG("Collecting garbage: done");
            }
            _DEBUG("Garbage collector stopped");
        });
    }
};

struct LocalClient::Impl {
    using GroupName = LocalClient::GroupName;
    using GroupRemoveHook = LocalClient::GroupRemoveHook;

    asio::executor _ex;
    util::Ed25519PublicKey _cache_pk;
    fs::path _cache_dir;
    LocalClient::opt_path _static_cache_dir;
    unique_ptr<cache::HttpStore> _http_store;
    boost::posix_time::time_duration _max_cached_age;
    Cancel _lifetime_cancel;
    GarbageCollector _gc;
    std::unique_ptr<DhtGroups> _groups;
    GroupRemoveHook _group_remove_hook;


    Impl( asio::executor exec_
        , util::Ed25519PublicKey& cache_pk
        , fs::path cache_dir
        , LocalClient::opt_path static_cache_dir
        , unique_ptr<cache::HttpStore> http_store_
        , boost::posix_time::time_duration max_cached_age)
        : _ex(move(exec_))
        , _cache_pk(cache_pk)
        , _cache_dir(move(cache_dir))
        , _static_cache_dir(std::move(static_cache_dir))
        , _http_store(move(http_store_))
        , _max_cached_age(max_cached_age)
        , _gc(*_http_store, [&] (auto rr, auto y) {
              return keep_cache_entry(move(rr), y);
          }, _ex)
        , _group_remove_hook([] (auto) {})
    {}

    GroupRemoveHook on_group_remove(GroupRemoveHook hook)
    {
        auto old_hook = move(_group_remove_hook);
        _group_remove_hook = move(hook);
        return old_hook;
    }

    GroupRemoveHook on_group_remove()
    {
        return on_group_remove([] (auto) {});
    }

    template<class Body>
    static
    boost::optional<util::HttpRequestByteRange> get_range(const http::request<Body>& rq)
    {
        auto rs = util::HttpRequestByteRange::parse(rq[http::field::range]);
        if (!rs) return boost::none;
        // XXX: We currently support max 1 rage in the request
        if ((*rs).size() != 1) return boost::none;
        return (*rs)[0];
    }

    bool serve( const http::request<http::empty_body>& req
              , GenericStream& sink
              , Cancel& cancel
              , Yield& yield)
    {
        sys::error_code ec;

        _YDEBUG(yield, "Start\n", req);

        // Usually we would
        // (1) check that the request matches our protocol version, and
        // (2) check that we can derive a key to look up the local cache.
        // However, we still want to blindly send a response we have cached
        // if the request looks like a Ouinet one and we can derive a key,
        // to help the requesting client get the result and other information
        // like a potential new protocol version.
        // The requesting client may choose to drop the response
        // or attempt to extract useful information from it.

        auto req_proto = req[http_::protocol_version_hdr];
        if (!boost::regex_match( req_proto.begin(), req_proto.end()
                               , http_::protocol_version_rx)) {
            _YDEBUG(yield, "Not a Ouinet request\n", req);
            handle_bad_request(sink, req, yield[ec]);
            return or_throw(yield, ec, req.keep_alive());
        }

        auto key = key_from_http_req(req);
        if (!key) {
            _YDEBUG(yield, "Cannot derive key from request\n", req);
            handle_bad_request(sink, req, yield[ec]);
            return or_throw(yield, ec, req.keep_alive());
        }

        _YDEBUG(yield, "Received request for ", *key);

        if (req.method() == http::verb::propfind) {
            _YDEBUG(yield, "Serving propfind for ", *key);
            auto hl = _http_store->load_hash_list
                (*key, cancel, static_cast<asio::yield_context>(yield[ec]));

            _YDEBUG(yield, "Load; ec=", ec);
            if (ec) {
                sys::error_code hnf_ec;
                handle_not_found(sink, req, yield[hnf_ec]);
                return or_throw(yield, hnf_ec, bool(!hnf_ec));
            }
            return_or_throw_on_error(yield, cancel, ec, false);
            yield[ec].tag("write_propfind").run([&] (auto y) {
                hl.write(sink, cancel, y);
            });
            _YDEBUG(yield, "Write; ec=", ec);
            return or_throw(yield, ec, bool(!ec));
        }

        cache::reader_uptr rr;

        auto range = get_range(req);

        if (range) {
            rr = _http_store->range_reader(*key, range->first, range->last, ec);
            assert(rr);
        } else {
            rr = _http_store->reader(*key, ec);
        }

        if (ec) {
            if (!cancel) {
                _YDEBUG(yield, "Not serving: ", *key, "; ec=", ec);
            }
            sys::error_code hnf_ec;
            handle_not_found(sink, req, yield[hnf_ec]);
            return or_throw(yield, hnf_ec, req.keep_alive());
        }

        _YDEBUG(yield, "Serving: ", *key);

        bool is_head_request = req.method() == http::verb::head;

        auto s = yield[ec].tag("read_hdr").run([&] (auto y) {
            return Session::create(move(rr), is_head_request, cancel, y);
        });

        if (ec) return or_throw(yield, ec, false);

        bool keep_alive = req.keep_alive() && s.response_header().keep_alive();

        Cancel timeout_cancel(cancel);
        yield[ec].tag("flush").run([&] (auto y) {
            // This short timeout will get reset with each successful send/recv operation,
            // so an exchange with no traffic at all does not get stuck for too long.
            auto op_wd = watch_dog( s.get_executor(), default_timeout::activity()
                                  , [&] { timeout_cancel(); });
            s.flush_response(timeout_cancel, y, [&op_wd, &sink] (auto&& part, auto& cc, auto yy) {
                sys::error_code ee;
                part.async_write(sink, cc, yy[ee]);
                return_or_throw_on_error(yy, cc, ee);
                op_wd.expires_after(default_timeout::activity());  // the part was successfully forwarded
            });
        });
        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;

        return or_throw(yield, ec, keep_alive);
    }

    std::size_t size( Cancel cancel
                    , asio::yield_context yield) const
    {
        return _http_store->size(cancel, yield);
    }

    void purge( Cancel cancel
              , asio::yield_context yield)
    {
        // TODO: avoid overlapping with garbage collector
        _DEBUG("Purging local cache...");

        sys::error_code ec;
        _http_store->for_each([&] (auto rr, auto y) {
            // TODO: Implement specific purge operations
            // for DHT groups and announcer
            // to avoid having to parse all stored heads.
            sys::error_code e;
            auto hdr = read_response_header(*rr, yield[e]);
            if (e) return false;
            auto key = hdr[http_::response_uri_hdr];
            if (key.empty()) return false;
            remove_cache_entry(key.to_string());
            return false;  // remove all entries
        }, cancel, yield[ec]);
        if (ec) {
            _ERROR("Purging local cache: failed;"
                   " ec=", ec);
            return or_throw(yield, ec);
        }

        _DEBUG("Purging local cache: done");
    }

    void handle_http_error( GenericStream& con
                          , const http::request<http::empty_body>& req
                          , http::status status
                          , const string& proto_error
                          , Yield yield)
    {
        auto res = util::http_error(req, status, OUINET_CLIENT_SERVER_STRING, proto_error);
        util::http_reply(con, res, static_cast<asio::yield_context>(yield));
    }

    void handle_bad_request( GenericStream& con
                           , const http::request<http::empty_body>& req
                           , Yield yield)
    {
        return handle_http_error(con, req, http::status::bad_request, "", yield);
    }

    void handle_not_found( GenericStream& con
                         , const http::request<http::empty_body>& req
                         , Yield yield)
    {
        return handle_http_error( con, req, http::status::not_found
                                , http_::response_error_hdr_retrieval_failed, yield);
    }

    Session load( const std::string& key
                , const GroupName& group
                , bool is_head_request
                , bool& is_complete
                , Cancel cancel
                , Yield yield)
    {
        sys::error_code ec;
        auto rr = _http_store->reader(key, ec);
        if (ec) return or_throw<Session>(yield, ec);
        auto rs = yield[ec].tag("read_hdr").run([&] (auto y) {
            return Session::create(move(rr), is_head_request, cancel, y);
        });
        return_or_throw_on_error(yield, cancel, ec, move(rs));

        auto rh = rs.response_header();
        rh.set( http_::response_source_hdr  // for agent
              , http_::response_source_hdr_local_cache);
        if (is_head_request) return rs;

        auto rs_sz = _http_store->body_size(key, ec);
        if (ec) {
            _YERROR(yield, "Failed to get body size of response in local cache; ec=", ec);
            return or_throw(yield, ec, move(rs));
        }

        auto data_size_sv = rh[http_::response_data_size_hdr];
        auto data_size_o = parse::number<std::size_t>(data_size_sv);
        is_complete = (data_size_o && rs_sz == *data_size_o);
        return rs;
    }

    void store( const std::string& key
              , const GroupName& group
              , http_response::AbstractReader& r
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;
        cache::KeepSignedReader fr(r);
        _http_store->store(key, fr, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        _groups->add(group, key, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);
    }

    http::response_header<>
    read_response_header( http_response::AbstractReader& reader
                        , asio::yield_context yield)
    {
        Cancel lc(_lifetime_cancel);

        sys::error_code ec;
        auto part = reader.async_read_part(lc, yield[ec]);
        if (!ec && !part)
            ec = sys::errc::make_error_code(sys::errc::no_message);
        return_or_throw_on_error(yield, lc, ec, http::response_header<>());
        auto head = part->as_head(); assert(head);
        return *head;
    }

    // Return maximum if not available.
    boost::posix_time::time_duration
    cache_entry_age(const http::response_header<>& head)
    {
        using ssecs = std::chrono::seconds;
        using bsecs = boost::posix_time::seconds;

        static auto max_age = bsecs(ssecs::max().count());

        auto ts_sv = util::http_injection_ts(head);
        if (ts_sv.empty()) return max_age;  // missing header or field
        auto ts_o = parse::number<ssecs::rep>(ts_sv);
        if (!ts_o) return max_age;  // malformed creation time stamp
        auto now = ssecs(std::time(nullptr));  // as done by injector
        auto age = now - ssecs(*ts_o);
        return bsecs(age.count());
    }

    inline
    void remove_cache_entry(const std::string& key)
    {
        auto empty_groups = _groups->remove(key);
        for (const auto& eg : empty_groups) _group_remove_hook(eg);
    }

    // Return whether the entry should be kept in storage.
    bool keep_cache_entry(cache::reader_uptr rr, asio::yield_context yield)
    {
        // This should be available to
        // allow removing keys of entries to be evicted.
        assert(_groups);

        sys::error_code ec;

        auto hdr = read_response_header(*rr, yield[ec]);
        if (ec) return or_throw<bool>(yield, ec);

        if (hdr[http_::protocol_version_hdr] != http_::protocol_version_hdr_current) {
            _WARN( "Cached response contains an invalid "
                 , http_::protocol_version_hdr
                 , " header field; removing");
            return false;
        }

        auto key = hdr[http_::response_uri_hdr];
        if (key.empty()) {
            _WARN( "Cached response does not contain a "
                 , http_::response_uri_hdr
                 , " header field; removing");
            return false;
        }

        auto age = cache_entry_age(hdr);
        if (age > _max_cached_age) {
            _DEBUG( "Cached response is too old; removing: "
                  , age, " > ", _max_cached_age
                  , "; uri=", key );
            remove_cache_entry(key.to_string());
            return false;
        }

        return true;
    }

    void load_stored_groups(asio::yield_context y)
    {
        static const auto groups_curver_subdir = "dht_groups";

        Cancel cancel(_lifetime_cancel);

        sys::error_code e;

        // Use static groups if its directory is provided.
        std::unique_ptr<BaseDhtGroups> static_groups;
        if (_static_cache_dir) {
            auto groups_dir = *_static_cache_dir / groups_curver_subdir;
            if (!is_directory(groups_dir)) {
                _ERROR("No groups of supported version under static cache, ignoring: ", *_static_cache_dir);
            } else {
                static_groups = load_static_dht_groups(move(groups_dir), _ex, cancel, y[e]);
                if (e) _ERROR("Failed to load static groups, ignoring: ", *_static_cache_dir);
            }
        }

        auto groups_dir = _cache_dir / groups_curver_subdir;
        _groups = static_groups
            ? load_backed_dht_groups(groups_dir, move(static_groups), _ex, cancel, y[e])
            : load_dht_groups(groups_dir, _ex, cancel, y[e]);

        if (cancel) e = asio::error::operation_aborted;
        if (e) return or_throw(y, e);

        _http_store->for_each([&] (auto rr, auto yield) {
            return keep_cache_entry(std::move(rr), yield);
        }, cancel, y[e]);
        if (e) return or_throw(y, e);

        // These checks are not bullet-proof, but they should catch some inconsistencies
        // between resource groups and the HTTP store.
        std::set<DhtGroups::ItemName> bad_items;
        std::set<DhtGroups::GroupName> bad_groups;
        for (auto& group_name : _groups->groups()) {
            unsigned good_items = 0;
            for (auto& group_item : _groups->items(group_name)) {
                // TODO: This implies opening all cache items (again for local cache), make lighter.
                sys::error_code ec;
                if (_http_store->reader(group_item, ec) != nullptr)
                    good_items++;
                else {
                    _WARN("Group resource missing from HTTP store: ", group_item, " (", group_name, ")");
                    bad_items.insert(group_item);
                }
            }
            if (good_items == 0) {
                _WARN("Not announcing group with no resources in HTTP store: ", group_name);
                bad_groups.insert(group_name);
            }
        }
        for (auto& group_name : bad_groups)
            _groups->remove_group(group_name);
        for (auto& item_name : bad_items)
            _groups->remove(item_name);
    }

    void stop() {
        _lifetime_cancel();
    }

    std::set<GroupName> get_groups() const {
        return _groups->groups();
    }
};

/* static */
std::unique_ptr<LocalClient>
LocalClient::build( asio::executor exec
                  , util::Ed25519PublicKey cache_pk
                  , fs::path cache_dir
                  , boost::posix_time::time_duration max_cached_age
                  , LocalClient::opt_path static_cache_dir
                  , LocalClient::opt_path static_cache_content_dir
                  , asio::yield_context yield)
{
    using LocalClientPtr = unique_ptr<LocalClient>;
    static const auto store_oldver_subdirs = {"data", "data-v1", "data-v2"};
    static const auto store_curver_subdir = "data-v3";

    sys::error_code ec;

    // Use a static HTTP store if its directories are provided.
    std::unique_ptr<BaseHttpStore> static_http_store;
    if (static_cache_dir) {
        assert(static_cache_content_dir);
        auto store_dir = *static_cache_dir / store_curver_subdir;
        fs::path canon_content_dir;
        if (!is_directory(store_dir)) {
            ec = asio::error::invalid_argument;
            _ERROR("No HTTP store of supported version under static cache, ignoring: ", *static_cache_dir);
        } else {
            canon_content_dir = fs::canonical(*static_cache_content_dir, ec);
            if (ec) _ERROR( "Failed to make static cache content directory canonical, ignoring: "
                          , *static_cache_content_dir);
        }
        if (!ec)
            // This static store should verify everything loaded from storage
            // (as its source may not be trustworthy),
            // which is not strictly needed for serving content to other clients
            // as they should verify on their own.
            // Nonetheless it may still help identify invalid or malicious content in it
            // before further propagating it.
            // The verification is also done for content retrieved for the local agent,
            // and in this case it is indeed desirable to do so.
            static_http_store = make_static_http_store( move(store_dir)
                                                      , move(canon_content_dir)
                                                      , cache_pk
                                                      , exec);
        ec = {};
    }

    // Remove obsolete stores.
    for (const auto& dirn : store_oldver_subdirs) {
        auto old_store_dir = cache_dir / dirn;
        if (!is_directory(old_store_dir)) continue;
        _INFO("Removing obsolete HTTP store...");
        fs::remove_all(old_store_dir, ec);
        if (ec) _ERROR("Removing obsolete HTTP store: failed; ec=", ec);
        else _INFO("Removing obsolete HTTP store: done");
        ec = {};
    }

    auto store_dir = cache_dir / store_curver_subdir;
    fs::create_directories(store_dir, ec);
    if (ec) return or_throw<LocalClientPtr>(yield, ec);
    auto http_store = static_http_store
        ? make_backed_http_store(move(store_dir), move(static_http_store), exec)
        : make_http_store(move(store_dir), exec);

    unique_ptr<Impl> impl(new Impl( move(exec)
                                  , cache_pk, move(cache_dir), std::move(static_cache_dir)
                                  , move(http_store), max_cached_age));

    impl->load_stored_groups(yield[ec]);
    if (ec) return or_throw<LocalClientPtr>(yield, ec);
    impl->_gc.start();

    return unique_ptr<LocalClient>(new LocalClient(move(impl)));
}

LocalClient::LocalClient(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

LocalClient::GroupRemoveHook
LocalClient::on_group_remove(LocalClient::GroupRemoveHook hook)
{
    return _impl->on_group_remove(move(hook));
}

LocalClient::GroupRemoveHook
LocalClient::on_group_remove()
{
    return _impl->on_group_remove();
}

Session LocalClient::load( const std::string& key
                         , const GroupName& group
                         , bool is_head_request
                         , bool& is_complete
                         , Cancel cancel, Yield yield)
{
    return _impl->load(key, group, is_head_request, is_complete, cancel, yield);
}

void LocalClient::store( const std::string& key
                       , const GroupName& group
                       , http_response::AbstractReader& r
                       , Cancel cancel
                       , asio::yield_context yield)
{
    _impl->store(key, group, r, cancel, yield);
}

bool LocalClient::serve( const http::request<http::empty_body>& req
                       , GenericStream& sink
                       , Cancel& cancel
                       , Yield yield)
{
    return _impl->serve(req, sink, cancel, yield);
}

std::size_t LocalClient::size( Cancel cancel
                             , asio::yield_context yield) const
{
    return _impl->size(cancel, yield);
}

void LocalClient::purge( Cancel cancel
                       , asio::yield_context yield)
{
    _impl->purge(cancel, yield);
}

std::set<LocalClient::GroupName> LocalClient::get_groups() const
{
    return _impl->get_groups();
}

LocalClient::~LocalClient()
{
    _impl->stop();
}
