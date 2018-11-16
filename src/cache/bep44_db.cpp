#include <iterator>

#include "bep44_db.h"
#include "../bittorrent/bencoding.h"
#include "../bittorrent/dht.h"
#include "../or_throw.h"

using namespace std;
using namespace ouinet;

namespace bt = bittorrent;

template<size_t N>
static
boost::string_view as_string_view(const array<uint8_t, N>& a)
{
    return boost::string_view((char*) a.data(), a.size());
}

Bep44ClientDb::Bep44ClientDb( bt::MainlineDht& bt_dht
                            , util::Ed25519PublicKey bt_pubkey)
    : _bt_dht(bt_dht)
    , _bt_pubkey(bt_pubkey)
    , _was_destroyed(make_shared<bool>(false))
{}


Bep44InjectorDb::Bep44InjectorDb( bt::MainlineDht& bt_dht
                                , util::Ed25519PrivateKey bt_privkey)
    : _bt_dht(bt_dht)
    , _bt_privkey(bt_privkey)
    , _was_destroyed(make_shared<bool>(false))
{}


static string find( bt::MainlineDht& dht
                  , util::Ed25519PublicKey pubkey
                  , const string& key
                  , Cancel& cancel
                  , asio::yield_context yield)
{
    sys::error_code ec;

    auto salt = util::sha1(key);

    auto cancel_handle = cancel.connect([] {
        assert(0 && "TODO: Bep44 index is not cancelable yet");
    });

    // TODO: Pass `cancel` to it
    auto opt_data = dht.mutable_get( pubkey
                                   , as_string_view(salt)
                                   , yield[ec]);
    
    if (!ec && !opt_data) {
        // TODO: This shouldn't happen (it does), the above
        // function should return an error if not successful.
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<string>(yield, ec);

    assert(opt_data->value.is_string());
    return *opt_data->value.as_string();
}


string Bep44ClientDb::find( const string& key
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    return ::find(_bt_dht, _bt_pubkey, key, cancel, yield);
}


string Bep44InjectorDb::find( const string& key
                            , Cancel& cancel
                            , asio::yield_context yield)
{
    return ::find(_bt_dht, _bt_privkey.public_key(), key, cancel, yield);
}


string Bep44ClientDb::insert_mapping( const string& ins_data
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

    return _bt_dht.mutable_put_start(item, yield).to_hex();
}

string Bep44InjectorDb::insert( string key
                              , string value
                              , asio::yield_context yield)
{
    using Time = boost::posix_time::ptime;

    /*
     * Use the sha1 of the URL as salt;
     * Use the timestamp as a version ID.
     */

    auto salt = util::sha1(key);

    Time unix_epoch(boost::gregorian::date(1970, 1, 1));
    Time ts = boost::posix_time::microsec_clock::universal_time();

    bt::MutableDataItem item;
    try {
        item = bt::MutableDataItem::sign( value
                                        , (ts - unix_epoch).total_milliseconds()
                                        , as_string_view(salt)
                                        , _bt_privkey);
    } catch(length_error) {
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


boost::asio::io_service& Bep44ClientDb::get_io_service()
{
    return _bt_dht.get_io_service();
}


boost::asio::io_service& Bep44InjectorDb::get_io_service()
{
    return _bt_dht.get_io_service();
}


Bep44ClientDb::~Bep44ClientDb()
{
    *_was_destroyed = true;
}


Bep44InjectorDb::~Bep44InjectorDb()
{
    *_was_destroyed = true;
}
