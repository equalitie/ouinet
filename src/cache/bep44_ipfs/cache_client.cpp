#include <asio_ipfs.h>
#include "cache_client.h"
#include "bep44_index.h"
#include "../cache_entry.h"
#include "descidx.h"
#include "http_desc.h"
#include "ipfs_util.h"
#include "../../or_throw.h"
#include "../../async_sleep.h"
#include "../../bittorrent/dht.h"
#include "../../logger.h"
#include "../../util/crypto.h"
#include "../../util/watch_dog.h"
#include "../../http_util.h"

using namespace std;
using namespace ouinet;
using namespace bep44_ipfs;

namespace asio = boost::asio;
namespace sys  = boost::system;
namespace bt   = ouinet::bittorrent;

unique_ptr<CacheClient>
CacheClient::build( asio::io_service& ios
                  , shared_ptr<bittorrent::MainlineDht> bt_dht
                  , boost::optional<util::Ed25519PublicKey> bt_pubkey
                  , fs::path path_to_repo
                  , bool autoseed_updated
                  , unsigned int bep44_index_capacity
                  , bool wait_for_ready
                  , Cancel& cancel
                  , asio::yield_context yield)
{
    using ClientP = unique_ptr<CacheClient>;

    sys::error_code ec;

    unique_ptr<asio_ipfs::node> ipfs_node;

    {
        auto slot = cancel.connect([&] {
            cerr << "TODO: Canceling CacheClient::build doesn't immediately stop "
                 << "IO tasks\n";
        });

        asio_ipfs::node::config cfg {
            .online       = true,
            // The default values 600/900/20 kill routers
            // See the Swarm section here for more info:
            // https://medium.com/textileio/tutorial-setting-up-an-ipfs-peer-part-iii-f5f43506874c
            .low_water    = 20,
            .high_water   = 50,
            .grace_period = 120
        };

        ipfs_node = asio_ipfs::node::build( ios
                                          , (path_to_repo/"ipfs").native()
                                          , cfg
                                          , yield[ec]);
    }

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<ClientP>(yield, ec);

    unique_ptr<Bep44ClientIndex> bep44_index;

    if (bt_pubkey) {
        bep44_index = Bep44ClientIndex::build(*bt_dht, *bt_pubkey
                                             , path_to_repo / "bep44-index"
                                             , bep44_index_capacity
                                             , cancel
                                             , yield[ec]);

        assert(!cancel || ec == asio::error::operation_aborted);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<ClientP>(yield, ec);
    }

    ClientP ret(new CacheClient( move(ipfs_node)
                               , std::move(bt_pubkey)
                               , std::move(bt_dht)
                               , std::move(bep44_index)
                               , move(path_to_repo)
                               , autoseed_updated));

    if (wait_for_ready) {
        ret->wait_for_ready(cancel, yield[ec]);
    }

    return or_throw(yield, ec, move(ret));
}

// private
CacheClient::CacheClient( std::unique_ptr<asio_ipfs::node> ipfs_node
                        , boost::optional<util::Ed25519PublicKey> bt_pubkey
                        , shared_ptr<bittorrent::MainlineDht> bt_dht
                        , unique_ptr<Bep44ClientIndex> bep44_index
                        , fs::path path_to_repo
                        , bool autoseed_updated)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(move(ipfs_node))
    , _bt_dht(move(bt_dht))
    , _index(move(bep44_index))
    , _store_scheduler(_ipfs_node->get_io_service(), 4)
    , _fetch_stored_scheduler(_ipfs_node->get_io_service(), 16)
{
    Bep44ClientIndex::UpdatedHook updated_hook([&, autoseed_updated]
                                               (auto o, auto n, auto& c, auto y) noexcept
    {
        // Returning false in this function avoids the republication of index entries
        // whose linked descriptors are missing or malformed,
        // or whose associated data cannot be retrieved.

        auto ipfs_load = IPFS_LOAD_FUNC(*_ipfs_node);
        sys::error_code ec;
        Cancel cancel(c);
        WatchDog wd( _ipfs_node->get_io_service()
                   // Even when not reseeding data, allow some time to reseed linked descriptor.
                   , autoseed_updated ? chrono::minutes(3) : chrono::seconds(30)  // TODO: adjust
                   , [&] { cancel(); });

        // Fetch and decode new descriptor.
        auto desc_data = descriptor::from_path(n, ipfs_load, cancel, y[ec]);
        if (ec || cancel) return false;
        auto desc = Descriptor::deserialize(desc_data);
        if (!desc) return false;

        if (!autoseed_updated) return true;  // do not care about data

        // Fetch data pointed by new descriptor.
        // TODO: check if it matches that of old descriptor
        auto data = ipfs_load(desc->body_link, cancel, y[ec]);
        if (cancel && !c) ec = asio::error::timed_out;

        LOG_DEBUG( "Fetch data from updated index entry:"
                 , " ec=\"", ec.message(), "\""
                 , " ipfs_cid=", desc->body_link," url=", desc->url);
        return !ec;
    });

    // Setup hooks.
    // Since indexes may start working right after construction,
    // setting hooks like this leaves a gap for
    // some updates to be detected by an index before the hook is set.
    // It is done like this to be able to create indexes in `build`
    // while retaining ownership of the IPFS node object here.
    _index->updated_hook(move(updated_hook));
}

CacheEntry CacheClient::load(const string& key, Cancel cancel, Yield yield)
{
    sys::error_code ec;
    auto slot = _fetch_stored_scheduler.wait_for_slot(cancel, yield);

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<CacheEntry>(yield, ec);

    auto cancel_con = _cancel.connect([&] { cancel(); });

    // XXX: get_content can accept string_view instead
    auto ret = get_content(key, cancel, yield[ec]);

    if (!ec) {
        // Prevent others from inserting ouinet headers.
        ret.second.response = util::remove_ouinet_fields(move(ret.second.response));

        // Add an injection identifier header
        // to enable the user to track injection state.
        ret.second.response.set(http_::response_injection_id_hdr, ret.first);
    }

    return or_throw(yield, ec, move(ret.second));
}

void CacheClient::store( const string& key
                       , Response& rs
                       , Cancel cancel_
                       , asio::yield_context yield)
{
    auto& ios = _ipfs_node->get_io_service();

    auto cancel_con = _cancel.connect([&] { cancel_(); });

    if (has_descriptor_hdr(rs)) {
        // Present if any insertion data does not contain the inlined descriptor
        // but a link to it: seed descriptor itself to distributed cache.
        asio::spawn(ios, [ desc_hdr = rs[http_::response_descriptor_hdr].to_string()
                         , key
                         , &cancel_
                         , this
                         ] (asio::yield_context yield) {
            Cancel cancel(cancel_);
            sys::error_code ec;
            seed_descriptor( key, desc_hdr
                           , _store_scheduler
                           , cancel, yield[ec]);
            LOG_DEBUG( "Index: seed descriptor for ", key
                     , " ec:\"", ec.message(), "\"");
        });
    }

    if (has_bep44_insert_hdr(rs)) {
        asio::spawn(ios, [ rs
                         , key
                         , this
                         , &cancel_
                         ] (asio::yield_context yield) {
            using namespace std::chrono;

            Cancel cancel(cancel_);

            steady_clock::duration scheduler_d = seconds(0)
                                 , bep44_d     = seconds(0)
                                 , ipfs_add_d  = seconds(0);

            sys::error_code ec;
            seed_response( key
                         , rs, _store_scheduler
                         , scheduler_d
                         , bep44_d
                         , ipfs_add_d
                         , cancel, yield[ec]);

            static constexpr auto secs = [](steady_clock::duration d) {
                return duration_cast<milliseconds>(d).count() / 1000.f;
            };

            // used by integration tests
            LOG_DEBUG("BEP44 index: insertion finished for ", key
                     , " ec:\"", ec.message(), "\" "
                     , "took scheduler:", secs(scheduler_d), "s, "
                           , "bep44m/put:", secs(bep44_d), "s, "
                           , "ipfs/add:", secs(ipfs_add_d), "s, "
                     , "remaining insertions: "
                           , _store_scheduler.slot_count(), " active, "
                           , _store_scheduler.waiter_count(), " pending");
        });
    }

    asio::spawn(ios,
        [ this, rs
        , &ios
        , &cancel_
        , key
        ] (asio::yield_context yield_) mutable {
            namespace err = asio::error;
            Cancel cancel(cancel_);

            Yield yield(ios, yield_, "Frontend");

            sys::error_code ec;

            // TODO: Be smarter about what we're storing here. I.e. don't
            // attempt to store what is currently being stored in another
            // coroutine or has been stored just recently.
            auto slot = _store_scheduler.wait_for_slot(cancel, yield[ec]);

            if (cancel) ec = err::operation_aborted;
            if (ec) return;

            // Seed content data itself.
            // TODO: Use the scheduler here to only do some max number
            // of `ipfs_add`s at a time. Also then trim that queue so
            // that it doesn't grow indefinitely.
            auto body_link = ipfs_add
                (beast::buffers_to_string(rs.body().data()), yield[ec]);

            auto inj_id = rs[http_::response_injection_id_hdr].to_string();
            rs = Response();  // drop heavy response body

            static const int max_attempts = 3;
            auto log_post_inject =
                [&] (int attempt, const string& msg){
                    // Adjust attempt number for meaningful 1-based logging.
                    attempt = (attempt < max_attempts) ? attempt + 1 : max_attempts;
                    LOG_DEBUG( "Post-inject lookup id=", inj_id
                             , " (", attempt, "/", max_attempts, "): "
                             , msg, "; key=", key);
                };

            // Retrieve the descriptor for the injection that we triggered
            // so that we help seed the URL->descriptor mapping too.
            // Try a few times to get the descriptor
            // (after some insertion delay, with exponential backoff).
            boost::optional<Descriptor> desc;
            int attempt = 0;
            for ( auto backoff = chrono::seconds(30);
                  attempt < max_attempts; backoff *= 2, ++attempt) {
                if (!async_sleep(ios, backoff, cancel, yield))
                    return;

                sys::error_code ec;
                auto desc_data = get_descriptor( key
                                               , cancel
                                               , yield[ec]);
                if (ec == err::not_found) {  // not (yet) inserted
                    log_post_inject(attempt, "not found, try again");
                    continue;
                } else if (ec) {  // some other error
                    log_post_inject(attempt, (boost::format("error=%s, giving up") % ec).str());
                    return;
                }

                desc = Descriptor::deserialize(desc_data);
                if (!desc) {  // maybe incompatible cache index
                    log_post_inject(attempt, "invalid descriptor, giving up");
                    return;
                }

                if (inj_id == desc->request_id)
                    break;  // found desired descriptor

                // different injection, try again
            }

            log_post_inject
                (attempt, desc ? ( boost::format("same_desc=%b same_data=%b")
                                 % (inj_id == desc->request_id)
                                 % (body_link == desc->body_link)).str()
                               : "did not find descriptor, giving up");
        });
}

string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    if (!_ipfs_node) return or_throw<string>(yield, asio::error::operation_not_supported);
    return _ipfs_node->add(data, yield);
}

string CacheClient::insert_mapping( const boost::string_view key
                                  , const std::string& ins_data
                                  , Cancel& cancel
                                  , boost::asio::yield_context yield)
{
    if (!_index) return or_throw<string>( yield
                                        , asio::error::operation_not_supported);

    return _index->insert_mapping(key, ins_data, cancel, yield);
}

string CacheClient::get_descriptor( const string& key, Cancel& cancel, Yield yield_)
{
    Yield yield = yield_.tag("CacheClient::get_descriptor");

    if (!_index) return or_throw<string>(yield, asio::error::not_found);

    sys::error_code ec;

    string desc_path = _index->find(key, cancel, yield[ec]);

    if (cancel || ec) {
        assert(ec != asio::error::operation_aborted || cancel);
        yield.log( "BEP44 lookup failed \"", ec.message(), "\""
                 , " key: \"", key, "\"");
    }

    return_or_throw_on_error(yield, cancel, ec, string());

    auto ret = descriptor_from_path(desc_path, cancel, yield[ec]);

    if (cancel || ec) {
        assert(ec != asio::error::operation_aborted || cancel);
        yield.log("Failed to resolve path \"", ec.message(), "\"");
    }

    return or_throw(yield, ec, move(ret));
}

string CacheClient::descriptor_from_path( const string& desc_path
                                        , Cancel& cancel
                                        , asio::yield_context yield)
{
    if (!_ipfs_node)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    return descriptor::from_path
        ( desc_path, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

pair<string, CacheEntry>
CacheClient::get_content(const string& key, Cancel& cancel, Yield yield_)
{
    Yield yield = yield_.tag("CacheClient::get_content");

    sys::error_code ec;

    string desc_data = get_descriptor(key, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    auto ret = descriptor::http_parse
        ( desc_data, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield[ec]);

    if (cancel) ec = asio::error::operation_aborted;

    if (ec) {
        yield.log("Failed at http_parse \"", ec.message(), "\"");
    }

    return or_throw(yield, ec, move(ret));
}

std::string CacheClient::ipfs_id() const
{
    assert(_ipfs_node);
    return _ipfs_node->id();
}

void
CacheClient::wait_for_ready(Cancel& cancel, asio::yield_context yield) const
{
    // TODO: Wait for IPFS cache to be ready, if needed.
    LOG_DEBUG("BEP44 index: waiting for BitTorrent DHT bootstrap...");
    _bt_dht->wait_all_ready(cancel, yield);
    LOG_DEBUG("BEP44 index: bootstrapped BitTorrent DHT");  // used by integration tests
}

CacheClient::~CacheClient() {
    _cancel();
}

/* static */
bool CacheClient::has_descriptor_hdr(const Response& rs)
{
    return !rs[http_::response_descriptor_hdr].empty();
}

/* static */
bool CacheClient::has_bep44_insert_hdr(const Response& rs)
{
    return !rs[http_::response_insert_hdr].empty();
}

void CacheClient::seed_descriptor( const std::string& target
                                 , const std::string& encoded_desc
                                 , Scheduler& scheduler
                                 , Cancel& cancel
                                 , asio::yield_context yield)
{
    sys::error_code ec;

    auto compressed_desc = util::base64_decode(encoded_desc);
    auto desc_data = util::zlib_decompress(compressed_desc, ec);
    if (ec) {
        LOG_WARN("Invalid descriptor data from injector; url=", target);
        return or_throw(yield, ec);
    }

    auto slot = scheduler.wait_for_slot(cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);

    ipfs_add(desc_data, yield[ec]);
    return or_throw(yield, ec);
}

void CacheClient::seed_response( const std::string& target
                               , const Response& rs
                               , Scheduler& scheduler
                               // These three are for debugging
                               , chrono::steady_clock::duration& scheduler_duration
                               , chrono::steady_clock::duration& bep44_duration
                               , chrono::steady_clock::duration& ipfs_add_duration
                               , Cancel& cancel
                               , asio::yield_context yield)
{
    using Clock = chrono::steady_clock;

    auto start = Clock::now();

    boost::string_view encoded_insd = rs[http_::response_insert_hdr];

    if (encoded_insd.empty()) return or_throw(yield, asio::error::no_data);

    string bep44_push_msg = util::base64_decode(encoded_insd);

    auto opt_item = bittorrent::MutableDataItem::bdecode(bep44_push_msg);

    if (!opt_item) {
        return or_throw(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;
    auto slot = scheduler.wait_for_slot(cancel, yield[ec]);

    scheduler_duration = Clock::now() - start;
    start = Clock::now();

    return_or_throw_on_error(yield, cancel, ec);

    insert_mapping(target, bep44_push_msg, cancel, yield[ec]);

    bep44_duration = Clock::now() - start;
    start = Clock::now();

    if (ec) return or_throw(yield, ec);

    auto desc_path = opt_item->value.as_string();
    assert(desc_path);

    auto desc = descriptor_from_path(*desc_path, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec);

    auto body_link = ipfs_add
        (beast::buffers_to_string(rs.body().data()), yield[ec]);

    ipfs_add_duration = Clock::now() - start;
    // TODO: (currently `desc` is just a json string, so can't do this
    // check directly)
    //assert(body_link == desc["value"]);
}

