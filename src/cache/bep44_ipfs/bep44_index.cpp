#include <iterator>
#include <sstream>
#include <tuple>
#include <nlohmann/json.hpp>

#include "bep44_index.h"
#include "../../util/file_io.h"
#include "../../logger.h"
#include "../../util/lru_cache.h"
#include "../../util/persistent_lru_cache.h"
#include "../../util/condition_variable.h"
#include "../../util/watch_dog.h"
#include "../../bittorrent/bencoding.h"
#include "../../bittorrent/dht.h"
#include "../../or_throw.h"
#include "../../defer.h"
#include "../../async_sleep.h"

using namespace std;
using namespace ouinet;
using namespace bep44_ipfs;

namespace bt = bittorrent;
namespace file_io = util::file_io;

using Clock = std::chrono::steady_clock;
using UpdatedHook = Bep44ClientIndex::UpdatedHook;

static uint64_t ms_since_epoch(Clock::time_point d)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(d.time_since_epoch()).count();
}

//--------------------------------------------------------------------
nlohmann::json entry_to_json( Clock::time_point t
                            , const string& key
                            , const bt::MutableDataItem& item)
{
    using util::bytes::to_hex;

    return nlohmann::json {
        { "key"             , key                                  },
        { "last_update"     , ms_since_epoch(t)                    },
        { "public_key"      , to_hex(item.public_key.serialize())  },
        { "salt"            , to_hex(item.salt)                    },
        { "value"           , to_hex(bencoding_encode(item.value)) },
        { "sequence_number" , item.sequence_number                 },
        { "signature"       , to_hex(item.signature)               },
    };
}

boost::optional<std::tuple<Clock::time_point, string, bt::MutableDataItem>>
entry_from_json(const nlohmann::json& json)
{
    using util::bytes::from_hex;
    using chrono::milliseconds;

    Clock::time_point last_update;
    string key;
    bt::MutableDataItem ret;

    try {
        auto pubkey = util::Ed25519PublicKey::from_hex(json["public_key"].get<string>());

        if (!pubkey) return boost::none;

        auto since_epoch = milliseconds(json["last_update"].get<uint64_t>());
        last_update = Clock::time_point() + since_epoch;

        key = json["key"].get<string>();

        ret.public_key      = *pubkey;
        ret.salt            = from_hex(json["salt"].get<string>());
        ret.value           = from_hex(json["value"].get<string>());
        ret.sequence_number = json["sequence_number"];

        auto sig = from_hex(json["signature"].get<string>());

        if (sig.size() != ret.signature.size()) return boost::none;

        ret.signature = util::bytes::to_array<uint8_t, 64>(sig);
    }
    catch (...) {
        return boost::none;
    }

    return make_tuple(last_update, move(key), move(ret));
}

//--------------------------------------------------------------------
template<size_t N>
static
boost::string_view as_string_view(const array<uint8_t, N>& a)
{
    return boost::string_view((char*) a.data(), a.size());
}


//--------------------------------------------------------------------
static bt::MutableDataItem find_bep44m( bt::MainlineDht& dht
                                      , util::Ed25519PublicKey pubkey
                                      , const string& salt
                                      , Cancel& cancel
                                      , asio::yield_context yield)
{
    sys::error_code ec;

    auto opt_data = dht.mutable_get(pubkey, salt, cancel, yield[ec]);

    if (!ec && !opt_data) {
        // TODO: This shouldn't happen (it does), the above
        // function should return an error if not successful.
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<bt::MutableDataItem>(yield, ec);

    return *opt_data;
}


//--------------------------------------------------------------------
class ouinet::bep44_ipfs::Bep44EntryUpdater
{
private:
    struct Entry {
        string key; // Mainly for debugging
        Clock::time_point last_update;
        bt::MutableDataItem data;

        template<class File>
        void write(File& f, Cancel& cancel, asio::yield_context yield) {
            auto s = entry_to_json(last_update, key, data).dump();
            file_io::write(f, asio::buffer(s), cancel, yield);
        }

        template<class File>
        void read(File& f, Cancel& cancel, asio::yield_context yield) {
            sys::error_code ec;
            auto size = file_io::file_remaining_size(f, ec);
            return_or_throw_on_error(yield, cancel, ec);

            string s(size, '\0');
            file_io::read(f, asio::buffer(s), cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec);

            try {
                auto json = nlohmann::json::parse(s);
                auto opt = entry_from_json(json);
                if (!opt) ec = asio::error::fault;
                return_or_throw_on_error(yield, cancel, ec);

                tie(last_update, key, data) = *opt;
            } catch (...) {
                return_or_throw_on_error(yield, cancel, asio::error::fault);
            }
        }
    };

public:
    using Lru = util::PersistentLruCache<Entry>;
    using LruPtr = unique_ptr<Lru>;

    UpdatedHook updated_hook;

public:
    Bep44EntryUpdater(bt::MainlineDht& dht, LruPtr lru)
        : _ios(dht.get_io_service())
        , _dht(dht)
        , _lru(move(lru))
        , _has_entries(_ios)
    {
        asio::spawn(_ios, [&] (asio::yield_context yield) { loop(yield); });
    }

    void insert( const boost::string_view key
               , bt::MutableDataItem data
               , Cancel& cancel_
               , asio::yield_context yield)
    {
        Cancel cancel;
        auto slot1 = cancel_.connect([&] { cancel(); });
        auto slot2 = _cancel.connect([&] { cancel(); });

        // TODO: The persistent LRU cache computes the SHA1 digest of this
        // to derive the file name.  Consider using `key` directly instead.
        // (It would break persistent data compatibility.)
        auto lru_key = data.salt;

        sys::error_code ec;

        _lru->insert( std::move(lru_key)
                    , Entry{ key.to_string()
                           , Clock::now() - chrono::minutes(15)
                           , std::move(data)}
                    , cancel
                    , yield[ec]);

        if (!slot2) {
            _has_entries.notify();
        }

        return or_throw(yield, ec);
    }

    ~Bep44EntryUpdater() {
        _cancel();
    }

private:

    void loop(asio::yield_context yield)
    {
        using namespace std::chrono;  // for `duration_cast<milliseconds>`

        Cancel cancel(_cancel);

        auto on_exit = defer([&] {
            LOG_DEBUG("Bep44EntryUpdater exited");
        });

        while (true) {
#ifndef NDEBUG
            LOG_DEBUG("Bep44EntryUpdater start new round");
#endif
            auto i = pick_entry_to_update();

            if (i == _lru->end()) {
#ifndef NDEBUG
                LOG_DEBUG("Bep44EntryUpdater nothing to update, waiting");
#endif
                Cancel tout(cancel);
                // Wait for new entries, but if none comes in a while,
                // check persisted entries again
                // in case any of them is now ready to update.
                WatchDog wd(_ios, chrono::seconds(15), [&]{ tout(); });
                sys::error_code ec_;
                _has_entries.wait(tout, yield[ec_]);
                if (cancel) return;
                continue;
            }

            auto loc = i.value();

            sys::error_code ec;

            LOG_DEBUG("Bep44EntryUpdater looking up bep44m ", loc.key);

            auto dht_data = find_bep44m(_dht
                                       , loc.data.public_key
                                       , loc.data.salt
                                       , cancel
                                       , yield[ec]);

            if (cancel) return;


            Clock::time_point next_update;
            if (ec) {
                LOG_DEBUG("Bep44EntryUpdater lookup failure ", ec.message());

                if (ec == asio::error::not_found) {
                    ec = sys::error_code();
                    _dht.mutable_put(loc.data, cancel, yield[ec]);
                    if (cancel) return;
                }

                LOG_DEBUG("Bep44EntryUpdater bep44m put"
                         , " result: ", ec.message()
                         , " cancel: ", bool(cancel));

                assert(!cancel || ec == asio::error::operation_aborted);
                if (ec && ec != asio::error::not_found && ec != asio::error::operation_aborted) {
                    // Some network error which may affect other entries as well,
                    // so do not move to the next one, just retry later.
                    async_sleep(_ios, chrono::seconds(5), cancel, yield);
                    if (cancel) return;
                    continue;
                }

                next_update = Clock::now();
            }
            else {
                auto dht_seq = dht_data.sequence_number
                   , loc_seq = loc.data.sequence_number;

                LOG_DEBUG("Bep44EntryUpdater lookup success"
                         , " loc_seq=", loc_seq
                         , " dht_seq=", dht_seq
                         , " salt=", util::bytes::to_hex(loc.data.salt)
                         , " ts1=", ms_since_epoch(loc.last_update)
                         , " updated_hook=", bool(updated_hook));

                if (dht_seq > loc_seq)
                {
                    bool do_repub = true;

                    if (updated_hook) {
                        do_repub = updated_hook( *(loc.data.value.as_string())
                                               , *(dht_data.value.as_string())
                                               , cancel, yield[ec]);
                        assert(!ec);  // should not propagate errors up
                        if (cancel) return;
                    }

                    LOG_DEBUG("Bep44EntryUpdater do_repub:", do_repub);

                    // Only republish updated index entries that the hook accepted.
                    if (do_repub) {
                        loc.data = move(dht_data);
                    };
                }

                next_update = Clock::now() - chrono::minutes(15);
            }

            // Regardless of whether we found the entry in the DHT or not,
            // we update the `last_update` ts just to make sure
            // we don't end up checking the same item over and over.
            loc.last_update = next_update;
            ec = sys::error_code();

            LOG_DEBUG( "Bep44EntryUpdater _lru->insert start"
                     , " ts2=", ms_since_epoch(next_update));

            _lru->insert(i.key(), move(loc), cancel, yield[ec]);

            LOG_DEBUG( "Bep44EntryUpdater _lru->insert end"
                     , " ec=", ec.message());

            if (cancel) return;
        }
    }

    Lru::iterator pick_entry_to_update() {
        auto oldest_i = _lru->end();

        for (auto i = _lru->begin(); i != _lru->end(); ++i) {
            if (!needs_update(i.value())) continue;

            if (oldest_i == _lru->end()) {
                oldest_i = i;
                continue;
            }

            if (i.value().last_update < oldest_i.value().last_update) {
                oldest_i = i;
            }
        }

        return oldest_i;
    }

    bool needs_update(const Entry& e) {
        return e.last_update < Clock::now() - chrono::minutes(30);
    }

private:
    asio::io_service& _ios;
    bt::MainlineDht& _dht;
    LruPtr _lru;
    Cancel _cancel;
    ConditionVariable _has_entries;
};


//--------------------------------------------------------------------
// static
unique_ptr<Bep44ClientIndex>
Bep44ClientIndex::build( bt::MainlineDht& bt_dht
                       , util::Ed25519PublicKey bt_pubkey
                       , const boost::filesystem::path& storage_path
                       , unsigned int capacity
                       , Cancel& cancel
                       , asio::yield_context yield)
{
    using Ret = unique_ptr<Bep44ClientIndex>;

    if (capacity == 0)
        return Ret(new Bep44ClientIndex(bt_dht, bt_pubkey, nullptr));

    sys::error_code ec;

    auto lru = Bep44EntryUpdater::Lru::load( bt_dht.get_io_service()
                                           , storage_path / "push-lru"
                                           , capacity
                                           , cancel
                                           , yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, Ret());

    auto updater = make_unique<Bep44EntryUpdater>(bt_dht, move(lru));

    return Ret(new Bep44ClientIndex(bt_dht, bt_pubkey, move(updater)));
}


// private
Bep44ClientIndex::Bep44ClientIndex( bt::MainlineDht& bt_dht
                                  , util::Ed25519PublicKey bt_pubkey
                                  , unique_ptr<Bep44EntryUpdater> updater)
    : _bt_dht(bt_dht)
    , _bt_pubkey(bt_pubkey)
    , _updater(move(updater))
{}

// public
void Bep44ClientIndex::updated_hook(UpdatedHook f) {
    if (!_updater) return;

    _updater->updated_hook = move(f);
}


//--------------------------------------------------------------------
// static
unique_ptr<Bep44InjectorIndex>
Bep44InjectorIndex::build( bt::MainlineDht& bt_dht
                         , util::Ed25519PrivateKey bt_privkey
                         , const boost::filesystem::path& storage_path
                         , unsigned int capacity
                         , Cancel& cancel
                         , asio::yield_context yield)
{
    using Ret = unique_ptr<Bep44InjectorIndex>;

    if (capacity == 0)
        return Ret(new Bep44InjectorIndex(bt_dht, bt_privkey, nullptr));

    sys::error_code ec;

    auto lru = Bep44EntryUpdater::Lru::load( bt_dht.get_io_service()
                                           , storage_path / "push-lru"
                                           , capacity
                                           , cancel
                                           , yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, Ret());

    auto updater = make_unique<Bep44EntryUpdater>(bt_dht, move(lru));

    return Ret(new Bep44InjectorIndex(bt_dht, bt_privkey, move(updater)));
}


Bep44InjectorIndex::Bep44InjectorIndex( bt::MainlineDht& bt_dht
                                      , util::Ed25519PrivateKey bt_privkey
                                      , unique_ptr<Bep44EntryUpdater> updater)
    : _bt_dht(bt_dht)
    , _bt_privkey(bt_privkey)
    , _updater(move(updater))
{}


//--------------------------------------------------------------------
string Bep44ClientIndex::find( const string& key
                             , Cancel& cancel_
                             , asio::yield_context yield)
{
    Cancel cancel;
    auto slot1 = cancel_.connect([&] { cancel(); });
    auto slot2 = _cancel.connect([&] { cancel(); });

    sys::error_code ec;
    auto data = ::find_bep44m( _bt_dht
                             , _bt_pubkey, bep44_salt_from_key(key)
                             , cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec2;
    if (_updater)
        _updater->insert(key, data, cancel, yield[ec2]);

    // Ignore all errors except operation_aborted
    if (ec2 == asio::error::operation_aborted) {
        return or_throw<string>(yield, ec2);
    }

    assert(data.value.is_string());
    return *data.value.as_string();
}


//--------------------------------------------------------------------
string Bep44InjectorIndex::find( const string& key
                               , Cancel& cancel_
                               , asio::yield_context yield)
{
    Cancel cancel;
    auto slot1 = cancel_.connect([&] { cancel(); });
    auto slot2 = _cancel.connect([&] { cancel(); });

    sys::error_code ec;
    auto data = ::find_bep44m( _bt_dht
                             , _bt_privkey.public_key(), bep44_salt_from_key(key)
                             , cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec2;
    if (_updater)
        _updater->insert(key, data, cancel, yield[ec2]);

    // Ignore all errors except operation_aborted
    if (ec2 == asio::error::operation_aborted) {
        return or_throw<string>(yield, ec2);
    }

    assert(data.value.is_string());
    return *data.value.as_string();
}


//--------------------------------------------------------------------
bittorrent::MutableDataItem
Bep44InjectorIndex::find_bep44m( boost::string_view key
                               , Cancel& cancel_
                               , asio::yield_context yield)
{
    Cancel cancel;
    auto slot1 = cancel_.connect([&] { cancel(); });
    auto slot2 = _cancel.connect([&] { cancel(); });

    return ::find_bep44m( _bt_dht
                        , _bt_privkey.public_key(), bep44_salt_from_key(key)
                        , cancel, yield);
}

//--------------------------------------------------------------------
string Bep44ClientIndex::insert_mapping( const boost::string_view key
                                       , const string& ins_data
                                       , Cancel& cancel
                                       , asio::yield_context yield)
{
    auto item = bt::MutableDataItem::bdecode(ins_data);

    if (!item) return or_throw<string>(yield, asio::error::invalid_argument);

    return insert_mapping(key, move(*item), cancel, yield);
}


string Bep44ClientIndex::insert_mapping( const boost::string_view key
                                       , bt::MutableDataItem item
                                       , Cancel& cancel_
                                       , asio::yield_context yield)
{
    Cancel cancel(_cancel);
    auto slot = cancel_.connect([&] { cancel(); });

    auto pk = item.public_key.serialize();
    auto salt = item.salt;

    sys::error_code ec;

    _bt_dht.mutable_put(item, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, string());

    // Ignore the error here
    if (_updater)
        _updater->insert(key, move(item), cancel, yield[ec]);

    return util::bytes::to_hex(util::sha1_digest(pk, salt));
}


bt::MutableDataItem
Bep44InjectorIndex::get_mutable_data_item( string key
                                         , string value
                                         , sys::error_code& ec)
{
    using Time = boost::posix_time::ptime;

    auto salt = bep44_salt_from_key(key);

    /*
     * Use the timestamp as a version ID.
     */

    Time unix_epoch(boost::gregorian::date(1970, 1, 1));
    Time ts = boost::posix_time::microsec_clock::universal_time();

    bt::MutableDataItem item;

    try {
        item = bt::MutableDataItem::sign( value
                                        , (ts - unix_epoch).total_milliseconds()
                                        , move(salt)
                                        , _bt_privkey);
    } catch(const length_error&) {
        ec = asio::error::message_size;
        return item;
    }

    return item;
}


string Bep44InjectorIndex::insert( string key
                                 , string value
                                 , asio::yield_context yield)
{
    sys::error_code ec;

    auto item = get_mutable_data_item(key, value, ec);

    if (ec) return or_throw<string>(yield, ec);

    Cancel cancel(_cancel);
    _bt_dht.mutable_put(item, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec_ignored;
    if (_updater)
        _updater->insert(key, item, cancel, yield[ec_ignored]);

    LOG_DEBUG("BEP44 index: inserted key=", key);  // used by integration tests

    return item.bencode();
}


string Bep44InjectorIndex::get_insert_message( string key
                                             , string value
                                             , sys::error_code& ec)
{
    return get_mutable_data_item(move(key), move(value), ec).bencode();
}


boost::asio::io_service& Bep44ClientIndex::get_io_service()
{
    return _bt_dht.get_io_service();
}


boost::asio::io_service& Bep44InjectorIndex::get_io_service()
{
    return _bt_dht.get_io_service();
}


Bep44ClientIndex::~Bep44ClientIndex()
{
    _cancel();
}


Bep44InjectorIndex::~Bep44InjectorIndex()
{
    _cancel();
}
