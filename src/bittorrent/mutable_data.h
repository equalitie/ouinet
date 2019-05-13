#pragma once

#include <chrono>
#include <deque>
#include <string>

#include "bencoding.h"
#include "node_id.h"

#include "../util/crypto.h"
#include "../util/signal.h"

namespace ouinet {
namespace bittorrent {

struct MutableDataItem {
    util::Ed25519PublicKey public_key;
    std::string salt;
    BencodedValue value;
    int64_t sequence_number;
    std::array<uint8_t, 64> signature;

    // Throws `std::length_error` if the value is too big.
    static MutableDataItem sign(
        BencodedValue value,
        int64_t sequence_number,
        boost::string_view salt,
        util::Ed25519PrivateKey private_key
    );

    bool verify() const;

    std::string bencode() const {
        using namespace std;

        auto pk = public_key.serialize();

        return bencoding_encode(BencodedMap{
            // cas is not compulsory
            // id depends on the publishing node
            { "k"   , string(begin(pk), end(pk)) },
            { "salt", salt },
            { "seq" , sequence_number },
            // token depends on the insertion
            { "sig" , string(begin(signature), end(signature)) },
            { "v"   , value }
        });
    }

    static
    boost::optional<MutableDataItem> bdecode(boost::string_view s) {
        using namespace std;

        // TODO: bencoding_decode should accept string_view
        auto ins = bencoding_decode(s);

        if (!ins || !ins->is_map()) {  // general format and type of data
            return boost::none;
        }

        MutableDataItem item;

        try {  // individual fields for mutable data item
            auto ins_map = ins->as_map();
            auto k = ins_map->at("k").as_string().value();

            if (k.size() != 32) return boost::none;

            array<uint8_t, 32> ka;
            copy(begin(k), end(k), begin(ka));

            item.public_key      = move(ka);
            item.salt            = ins_map->at("salt").as_string().value();
            item.value           = ins_map->at("v");
            item.sequence_number = ins_map->at("seq").as_int().value();

            auto sig = ins_map->at("sig").as_string().value();

            if (sig.size() != item.signature.size()) return boost::none;

            copy(begin(sig), end(sig), begin(item.signature));
        }
        catch (const exception&) {
            return boost::none;
        }

        if (!item.verify()) {  // mutable data item signature
            return boost::none;
        }

        return item;
    }
};

} // bittorrent namespace
} // ouinet namespace
