#include "mutable_data.h"

#include "../util/bytes.h"
#include "../util/crypto.h"

namespace ouinet {
namespace bittorrent {

static std::string mutable_data_signature_buffer(
    const BencodedValue& data,
    boost::string_view salt,
    int64_t sequence_number
) {
    std::string encoded_data = bencoding_encode(data);

    /*
     * It's not safe to assume storing more than 1000 bytes will succeed,
     * according to http://bittorrent.org/beps/bep_0044.html#messages
     */
    if (encoded_data.length() > 1000)
        throw std::length_error("data too big for DHT storage");

    /*
     * Low-level buffer computation is mandated by
     * http://bittorrent.org/beps/bep_0044.html#signature-verification
     *
     * This is a concatenation of three key/value pairs encoded as they are in
     * a BencodedMap, but in a nonstandard way, and as specified not actually
     * implemented using the BencodedMap logic.
     */
    std::string signature_buffer;
    if (!salt.empty()) {
        signature_buffer += "4:salt";
        signature_buffer += std::to_string(salt.size());
        signature_buffer += ":";
        signature_buffer += salt.to_string();
    }
    signature_buffer += "3:seqi";
    signature_buffer += std::to_string(sequence_number);
    signature_buffer += "e1:v";
    signature_buffer += encoded_data;
    return signature_buffer;
}

MutableDataItem MutableDataItem::sign(
    BencodedValue value,
    int64_t sequence_number,
    boost::string_view salt,
    util::Ed25519PrivateKey private_key
) {
    MutableDataItem output{
        private_key.public_key(),
        salt.to_string(),
        value,
        sequence_number
    };

    output.signature = private_key.sign(
        mutable_data_signature_buffer(value, salt, sequence_number)
    );

    return output;
}

bool MutableDataItem::verify() const
{
    return public_key.verify(
        mutable_data_signature_buffer(
            value,
            salt,
            sequence_number
        ),
        signature
    );
}

} // bittorrent namespace
} // ouinet namespace

