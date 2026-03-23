#define BOOST_TEST_MODULE signing
#include <boost/test/included/unit_test.hpp>

#include "util/crypto.cpp"
#include "util/sign.cpp"

using namespace ouinet;

BOOST_AUTO_TEST_CASE(start_with_gcrypt) {
    std::string message = "The quick brown fox";

    auto gcr_sk = util::Ed25519PrivateKey::generate();
    auto gcr_pk = gcr_sk.public_key();
    auto gcr_sig = gcr_sk.sign(message);

    auto ssl_sk = sign::SecretKey(gcr_sk.serialize());
    auto ssl_pk = ssl_sk.public_key();
    auto ssl_sig = ssl_sk.sign(message);

    BOOST_CHECK_EQUAL(ssl_sk.to_hex(), util::bytes::to_hex(gcr_sk.serialize()));
    BOOST_CHECK_EQUAL(ssl_pk.to_hex(), util::bytes::to_hex(gcr_pk.serialize()));
    BOOST_CHECK_EQUAL(ssl_sig.to_hex(), util::bytes::to_hex(gcr_sig));

    BOOST_CHECK(gcr_pk.verify(message, ssl_sig.bytes));
    BOOST_CHECK(ssl_pk.verify(message, sign::Signature(gcr_sig)));
}

BOOST_AUTO_TEST_CASE(start_with_openssl) {
    std::string message = "The quick brown fox";

    auto ssl_sk = sign::SecretKey::generate();
    auto ssl_pk = ssl_sk.public_key();
    auto ssl_sig = ssl_sk.sign(message);

    auto gcr_sk = util::Ed25519PrivateKey(ssl_sk.to_bytes());
    auto gcr_pk = gcr_sk.public_key();
    auto gcr_sig = gcr_sk.sign(message);

    BOOST_CHECK_EQUAL(ssl_sk.to_hex(), util::bytes::to_hex(gcr_sk.serialize()));
    BOOST_CHECK_EQUAL(ssl_pk.to_hex(), util::bytes::to_hex(gcr_pk.serialize()));
    BOOST_CHECK_EQUAL(ssl_sig.to_hex(), util::bytes::to_hex(gcr_sig));

    BOOST_CHECK(gcr_pk.verify(message, ssl_sig.bytes));
    BOOST_CHECK(ssl_pk.verify(message, sign::Signature(gcr_sig)));
}
