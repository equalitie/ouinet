#include "bep44_db.h"
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


void Bep44InjectorDb::insert( string key
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

    auto item = bt::MutableDataItem::sign( value
                                         , (ts - unix_epoch).total_milliseconds()
                                         , as_string_view(salt)
                                         , _bt_privkey);

    _bt_dht.mutable_put_start(item, yield);
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
