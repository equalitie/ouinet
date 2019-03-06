#include <iterator>
#include <json.hpp>

#include "bep44_index.h"
#include "../logger.h"
#include "../util/lru_cache.h"
#include "../util/persistent_lru_cache.h"
#include "../bittorrent/bencoding.h"
#include "../bittorrent/dht.h"
#include "../or_throw.h"
#include "../defer.h"
#include "../async_sleep.h"

using namespace std;
using namespace ouinet;

namespace bt = bittorrent;

//--------------------------------------------------------------------
nlohmann::json to_json(const bt::MutableDataItem& item)
{
    using util::bytes::to_hex;

    return nlohmann::json {
        { "public_key"      , to_hex(item.public_key.serialize())  },
        { "salt"            , to_hex(item.salt)                    },
        { "value"           , to_hex(bencoding_encode(item.value)) },
        { "sequence_number" , item.sequence_number                 },
        { "signature"       , to_hex(item.signature)               },
    };
}

boost::optional<bt::MutableDataItem> from_json(const nlohmann::json& json)
{
    using util::bytes::from_hex;

    bt::MutableDataItem ret;

    try {
        auto pubkey = util::Ed25519PublicKey::from_hex(json["public_key"].get<string>());

        if (!pubkey) return boost::none;

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

    return ret;
}

//--------------------------------------------------------------------
template<size_t N>
static
boost::string_view as_string_view(const array<uint8_t, N>& a)
{
    return boost::string_view((char*) a.data(), a.size());
}


//--------------------------------------------------------------------
static bt::MutableDataItem find( bt::MainlineDht& dht
                               , util::Ed25519PublicKey pubkey
                               , const string& salt
                               , Cancel& cancel
                               , asio::yield_context yield)
{
    sys::error_code ec;

    auto opt_data = dht.mutable_get(pubkey, salt, yield[ec], cancel);
    
    if (!ec && !opt_data) {
        // TODO: This shouldn't happen (it does), the above
        // function should return an error if not successful.
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<bt::MutableDataItem>(yield, ec);

    return *opt_data;
}


//--------------------------------------------------------------------
class ouinet::Bep44EntryUpdater
{
public:
    // Arbitrarily chosen
    static const size_t CAPACITY = 1000;

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Clock::time_point last_update;
    };

    using Entries = util::LruCache<string, Entry>;
    using LruPtr = unique_ptr<util::PersistentLruCache>;

public:
    Bep44EntryUpdater(bt::MainlineDht& dht, LruPtr lru)
        : _ios(dht.get_io_service())
        , _dht(dht)
        , _entries(CAPACITY)
        , _lru(move(lru))
    {
        start();
    }

    void insert( const bt::MutableDataItem& data
               , Cancel& cancel
               , asio::yield_context yield)
    {
        auto slot = _cancel.connect([&] { cancel(); });

        sys::error_code ec;

        _lru->insert(data.salt, to_json(data).dump(), cancel, yield[ec]);

        return_or_throw_on_error(yield, cancel, ec);
        
        _entries.put(data.salt, Entry{ Clock::now() - chrono::minutes(15)});

        return or_throw(yield, ec);
    }

    ~Bep44EntryUpdater() {
        _cancel();
    }

private:
    void start()
    {
        asio::spawn(_ios, [&] (asio::yield_context yield) {
            Cancel cancel(_cancel);

            while (true) {
                auto i = pick_entry_to_update();

                if (i == _entries.end()) {
                    async_sleep(_ios, chrono::minutes(1), cancel, yield);
                    if (cancel) return;
                    continue;
                }

                auto& old = i->second;

                sys::error_code ec;

                util::PersistentLruCache::iterator old_data_i = _lru->find(i->first);

                if (old_data_i == _lru->end()) {
                    _entries.erase(i);
                    continue;
                }

                auto old_data_s = old_data_i.value(cancel, yield[ec]);

                if (cancel) return;

                if (ec) {
                    _entries.erase(i);
                    continue;
                }

                auto old_data = from_json(old_data_s);

                if (!old_data) {
                    _entries.erase(i);
                    continue;
                }

                auto new_data = find(_dht
                                    , old_data->public_key
                                    , old_data->salt
                                    , cancel
                                    , yield[ec]);

                if (cancel) return;

                if (ec) {
                    if (ec == asio::error::not_found) {
                        _dht.mutable_put(*old_data, cancel, yield[ec]);
                        if (cancel) return;
                        old.last_update = Clock::now();
                    } else {
                        // Some network error
                        async_sleep(_ios, chrono::seconds(5), cancel, yield);
                        if (cancel) return;
                    }
                } else {
                    if (new_data.sequence_number > old_data->sequence_number)
                    {
                        // TODO: Store new data
                        old.last_update = Clock::now() - chrono::minutes(15);
                    }
                }
            }
        });
    }

    Entries::iterator pick_entry_to_update() {
        auto oldest_i = _entries.end();

        for (auto i = _entries.begin(); i != _entries.end(); ++i) {
            if (!needs_update(i->second)) continue;

            if (oldest_i == _entries.end()) {
                oldest_i = i;
                continue;
            }

            if (i->second.last_update < oldest_i->second.last_update) {
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
    Entries _entries;
    LruPtr _lru;
    Cancel _cancel;
};


//--------------------------------------------------------------------
// static
unique_ptr<Bep44ClientIndex>
Bep44ClientIndex::build( bt::MainlineDht& bt_dht
                       , util::Ed25519PublicKey bt_pubkey
                       , const boost::filesystem::path& storage_path
                       , Cancel& cancel
                       , asio::yield_context yield)
{
    using Ret = unique_ptr<Bep44ClientIndex>;

    sys::error_code ec;

    auto lru = util::PersistentLruCache::load( bt_dht.get_io_service()
                                             , storage_path / "push-lru"
                                             , Bep44EntryUpdater::CAPACITY
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


//--------------------------------------------------------------------
// static
unique_ptr<Bep44InjectorIndex>
Bep44InjectorIndex::build( bt::MainlineDht& bt_dht
                         , util::Ed25519PrivateKey bt_privkey
                         , const boost::filesystem::path& storage_path
                         , Cancel& cancel
                         , asio::yield_context yield)
{
    using Ret = unique_ptr<Bep44InjectorIndex>;

    sys::error_code ec;

    auto lru = util::PersistentLruCache::load( bt_dht.get_io_service()
                                             , storage_path / "push-lru"
                                             , Bep44EntryUpdater::CAPACITY
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
    auto data = ::find( _bt_dht
                      , _bt_pubkey, bep44_salt_from_key(key)
                      , cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec2;
    _updater->insert(data, cancel, yield[ec2]);

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
    auto data = ::find( _bt_dht
                      , _bt_privkey.public_key(), bep44_salt_from_key(key)
                      , cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec2;
    _updater->insert(data, cancel, yield[ec2]);

    // Ignore all errors except operation_aborted
    if (ec2 == asio::error::operation_aborted) {
        return or_throw<string>(yield, ec2);
    }

    assert(data.value.is_string());
    return *data.value.as_string();
}


//--------------------------------------------------------------------
string Bep44ClientIndex::insert_mapping( const string& ins_data
                                       , asio::yield_context yield)
{
    auto ins = bt::bencoding_decode(ins_data);
    if (!ins || !ins->is_map()) {  // general format and type of data
        return or_throw<string>(yield, asio::error::invalid_argument);
    }

    auto ins_map = ins->as_map();
    bt::MutableDataItem item;
    try {  // individual fields for mutable data item
        auto k = ins_map->at("k").as_string().value();
        if (k.size() == 32) {  // or let verification fail
            array<uint8_t, 32> ka;
            copy(begin(k), end(k), begin(ka));
            item.public_key = move(ka);
        }
        item.salt = ins_map->at("salt").as_string().value();
        item.value = ins_map->at("v");
        item.sequence_number = ins_map->at("seq").as_int().value();
        auto sig = ins_map->at("sig").as_string().value();
        if (sig.size() == item.signature.size())  // or let verification fail
            copy(begin(sig), end(sig), begin(item.signature));
    } catch (const exception& e) {
        return or_throw<string>(yield, asio::error::invalid_argument);
    }
    if (!item.verify()) {  // mutable data item signature
        return or_throw<string>(yield, asio::error::invalid_argument);
    }

    // TODO: Replace this with the Updater logic
    return _bt_dht.mutable_put_start(item).to_hex();
}

string Bep44InjectorIndex::insert( string key
                                 , string value
                                 , asio::yield_context yield)
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
        return or_throw<string>(yield, asio::error::message_size);
    }

    sys::error_code ec;

    Cancel cancel(_cancel);
    _bt_dht.mutable_put(item, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    sys::error_code ec_ignored;
    _updater->insert(item, cancel, yield[ec_ignored]);

    LOG_DEBUG("BEP44 index: inserted key=", key);  // used by integration tests

    // We follow the names used in the BEP44 document.
    auto pk = item.public_key.serialize();
    return bt::bencoding_encode(bt::BencodedMap{
        // cas is not compulsory
        // id depends on the publishing node
        { "k"   , string(begin(pk), end(pk)) },
        { "salt", item.salt },
        { "seq" , item.sequence_number },
        // token depends on the insertion
        { "sig" , string(begin(item.signature), end(item.signature)) },
        { "v"   , value }
    });
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
}


Bep44InjectorIndex::~Bep44InjectorIndex()
{
}
