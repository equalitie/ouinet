#include "local_client.h"

#include "dht_groups.h"
#include "http_store.h"


#define _LOGPFX "cache/local: "
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
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

    //TODO impl->announce_stored_data(yield[ec]);
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
