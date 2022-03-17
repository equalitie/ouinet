#include "local_client.h"

#include "dht_groups.h"
#include "http_store.h"


#define _LOGPFX "cache/local: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)


using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

namespace fs = boost::filesystem;


struct LocalClient::Impl {
    asio::executor _ex;
    util::Ed25519PublicKey _cache_pk;
    fs::path _cache_dir;
    LocalClient::opt_path _static_cache_dir;
    unique_ptr<cache::HttpStore> _http_store;
    boost::posix_time::time_duration _max_cached_age;
    Cancel _lifetime_cancel;
    std::unique_ptr<DhtGroups> _dht_groups;


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
    {}

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
        auto empty_groups = _dht_groups->remove(key);
        //TODO for (const auto& eg : empty_groups) on_remove_group_hook(eg);
    }

    // Return whether the entry should be kept in storage.
    bool keep_cache_entry(cache::reader_uptr rr, asio::yield_context yield)
    {
        // This should be available to
        // allow removing keys of entries to be evicted.
        assert(_dht_groups);

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

        // Use static DHT groups if its directory is provided.
        std::unique_ptr<BaseDhtGroups> static_dht_groups;
        if (_static_cache_dir) {
            auto groups_dir = *_static_cache_dir / groups_curver_subdir;
            if (!is_directory(groups_dir)) {
                _ERROR("No DHT groups of supported version under static cache, ignoring: ", *_static_cache_dir);
            } else {
                static_dht_groups = load_static_dht_groups(move(groups_dir), _ex, cancel, y[e]);
                if (e) _ERROR("Failed to load static DHT groups, ignoring: ", *_static_cache_dir);
            }
        }

        auto groups_dir = _cache_dir / groups_curver_subdir;
        _dht_groups = static_dht_groups
            ? load_backed_dht_groups(groups_dir, move(static_dht_groups), _ex, cancel, y[e])
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
        for (auto& group_name : _dht_groups->groups()) {
            unsigned good_items = 0;
            for (auto& group_item : _dht_groups->items(group_name)) {
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
            _dht_groups->remove_group(group_name);
        for (auto& item_name : bad_items)
            _dht_groups->remove(item_name);
    }

    void stop() {
        _lifetime_cancel();
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
    //TODO impl->_gc.start();

    return unique_ptr<LocalClient>(new LocalClient(move(impl)));
}

LocalClient::LocalClient(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

LocalClient::~LocalClient()
{
    _impl->stop();
}
