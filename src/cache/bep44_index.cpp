#include <iterator>

#include "bep44_index.h"
#include "../util/lru_cache.h"
#include "../bittorrent/bencoding.h"
#include "../bittorrent/dht.h"
#include "../or_throw.h"
#include "../defer.h"
#include "../async_sleep.h"

using namespace std;
using namespace ouinet;

namespace bt = bittorrent;

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

    auto opt_data = dht.mutable_get( pubkey, salt
                                   , yield[ec], cancel);
    
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
    using Clock = std::chrono::steady_clock;

    // Arbitrarily chosen
    static const size_t CAPACITY = 1000;

    struct Entry {
        bt::MutableDataItem data;
        Clock::time_point last_update;
    };

    using Entries = util::LruCache<string, Entry>;

public:
    Bep44EntryUpdater(bt::MainlineDht& dht)
        : _ios(dht.get_io_service())
        , _dht(dht)
        , _entries(CAPACITY)
    {
        start();
    }

    void insert(bt::MutableDataItem data)
    {
        // For different entries *signed by the same key*,
        // their salt is the differentiating key.
        _entries.put(data.salt, Entry{ move(data)
                                     , Clock::now() - chrono::minutes(15)});
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

                auto new_data = find(_dht
                                    , old.data.public_key
                                    , i->first  // the salt
                                    , cancel
                                    , yield[ec]);

                if (cancel) return;

                if (ec) {
                    if (ec == asio::error::not_found) {
                        _dht.mutable_put(old.data, cancel, yield[ec]);
                        if (cancel) return;
                        old.last_update = Clock::now();
                    } else {
                        // Some network error
                        async_sleep(_ios, chrono::seconds(5), cancel, yield);
                        if (cancel) return;
                    }
                } else {
                    if (new_data.sequence_number > old.data.sequence_number)
                    {
                        old.data = move(new_data);
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
    Cancel _cancel;
};


//--------------------------------------------------------------------
Bep44ClientIndex::Bep44ClientIndex( bt::MainlineDht& bt_dht
                                  , util::Ed25519PublicKey bt_pubkey)
    : _bt_dht(bt_dht)
    , _bt_pubkey(bt_pubkey)
    , _updater(new Bep44EntryUpdater(bt_dht))
    , _was_destroyed(make_shared<bool>(false))
{}


Bep44InjectorIndex::Bep44InjectorIndex( bt::MainlineDht& bt_dht
                                      , util::Ed25519PrivateKey bt_privkey)
    : _bt_dht(bt_dht)
    , _bt_privkey(bt_privkey)
    , _updater(new Bep44EntryUpdater(bt_dht))
    , _was_destroyed(make_shared<bool>(false))
{}


string Bep44ClientIndex::find( const string& key
                             , Cancel& cancel
                             , asio::yield_context yield)
{
    sys::error_code ec;
    auto data = ::find( _bt_dht
                      , _bt_pubkey, bep44_salt_from_key(key)
                      , cancel, yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    _updater->insert(data);

    assert(data.value.is_string());
    return *data.value.as_string();
}


string Bep44InjectorIndex::find( const string& key
                               , Cancel& cancel
                               , asio::yield_context yield)
{
    sys::error_code ec;
    auto data = ::find( _bt_dht
                      , _bt_privkey.public_key(), bep44_salt_from_key(key)
                      , cancel, yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    // TODO: Don't do this once we have a bootstrapped network.
    _updater->insert(data);

    assert(data.value.is_string());
    return *data.value.as_string();
}


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

    _bt_dht.mutable_put_start(item, yield);

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
    *_was_destroyed = true;
}


Bep44InjectorIndex::~Bep44InjectorIndex()
{
    *_was_destroyed = true;
}
