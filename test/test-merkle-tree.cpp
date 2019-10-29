#define BOOST_TEST_MODULE merkle_tree
#include <boost/test/included/unit_test.hpp>

#include "util/merkle_tree.h"
#include "util/hash.h"
#include "util/crypto.h"

#include <iostream>

namespace std {

inline
std::ostream& operator<<( std::ostream& os
                        , const std::array<unsigned char, 64>& sig) {
    static const char hex[] = { '0', '1', '2', '3'
                              , '4', '5', '6', '7'
                              , '8', '9', 'a', 'b'
                              , 'c', 'd', 'e', 'f' };
    os << "SIG(";
    for (auto c : sig) os << hex[(c >> 4) & 0xf] << hex[c & 0xf];
    return os << ")";
}

} // std namespace

BOOST_AUTO_TEST_SUITE(ouinet_merkle_tree)

using namespace std;
using namespace ouinet;
using namespace ouinet::util;

struct MockHash {
    using digest_type = string;

    static
    digest_type digest(string s) {
        return "(" + s + ")";
    }

    static
    digest_type digest(string s1, string s2) {
        return "(" + s1 + "+" + s2 + ")";
    }
};

struct MockCrypto {
    struct PrivateKey {
        using sig_array_t = string;

        static PrivateKey generate() { return {}; }

        sig_array_t sign(const string& s) const {
            return "SIG(" + s + ")";
        }
    };
};

//using M = MerkleTree<SHA512, Ed25519>;
using M = MerkleTree<MockHash, MockCrypto>;

BOOST_AUTO_TEST_CASE(merkle_tree) {
    crypto_init();

    using Hash = M::Hash;

    auto priv_key = M::PrivateKey::generate();

    {
        M m;
        auto s = m.append_and_sign(Hash::digest("n0"), priv_key);
        BOOST_REQUIRE_EQUAL(s, priv_key.sign(Hash::digest("n0")));
    }

    {
        /*
         *      n10
         *      / \
         *    n00 n01
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n10 = Hash::digest(n00, n01);

        m.append_and_sign(n00, priv_key);
        auto s = m.append_and_sign(n01, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n10));
    }

    {
        /*
         *        n20
         *        / \
         *      n10  \
         *      / \   \
         *    n00 n01 n02
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n10 = Hash::digest(n00, n01);
        auto n20 = Hash::digest(n10, n02);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        auto s = m.append_and_sign(n02, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n20));
    }

    {
        /*
         *         n20
         *         / \
         *        /   \
         *     n10     n11
         *     / \     / \
         *   n00 n01 n02 n03
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n20 = Hash::digest(n10, n11);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        auto s = m.append_and_sign(n03, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n20));
    }

    {
        /*
         *            n30
         *            / \
         *           /   \
         *         n20    \
         *         / \     \
         *        /   \     \
         *     n10     n11   \
         *     / \     / \    \
         *   n00 n01 n02 n03  n04 
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n20 = Hash::digest(n10, n11);
        auto n30 = Hash::digest(n20, n04);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        auto s = m.append_and_sign(n04, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n30));
    }

    {
        /*
         *             n30
         *             / \
         *            /   \
         *           /     \
         *         n20      \
         *         / \       \
         *        /   \       \
         *     n10     n11     n12
         *     / \     / \    /  \
         *   n00 n01 n02 n03 n04 n05
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n20 = Hash::digest(n10, n11);
        auto n30 = Hash::digest(n20, n12);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        auto s = m.append_and_sign(n05, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n30));
    }

    {
        /*
         *                n30
         *               /   \
         *              /     \
         *             /       \
         *            /         \
         *           /           \
         *         n20           n21
         *         / \           / \
         *        /   \         /   \
         *     n10     n11     n12   \
         *     / \     / \    /  \    \
         *   n00 n01 n02 n03 n04 n05  n06
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n06);
        auto n30 = Hash::digest(n20, n21);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        auto s = m.append_and_sign(n06, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n30));
    }

    {
        /*
         *                n30
         *               /   \
         *              /     \
         *             /       \
         *            /         \
         *           /           \
         *         n20            n21
         *         / \           /   \
         *        /   \         /     \
         *     n10     n11     n12     n13
         *     / \     / \    /  \    /  \
         *   n00 n01 n02 n03 n04 n05 n06 n07
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n30 = Hash::digest(n20, n21);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        auto s = m.append_and_sign(n07, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n30));
    }

    {
        /*
         *                    n40
         *                   /   \
         *                  /     \
         *                n30      \
         *               /   \      \
         *              /     \      \
         *             /       \      \
         *            /         \      \
         *           /           \      \
         *         n20            n21    \
         *         / \           /   \    \
         *        /   \         /     \    \
         *     n10     n11     n12     n13  \
         *     / \     / \    /  \    /  \   \
         *   n00 n01 n02 n03 n04 n05 n06 n07 n08
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n08 = Hash::digest("n08");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n30 = Hash::digest(n20, n21);
        auto n40 = Hash::digest(n30, n08);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        m.append_and_sign(n07, priv_key);
        auto s = m.append_and_sign(n08, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n40));
    }

    {
        /*
         *                     n40
         *                    /    \
         *                   /      \
         *                  /        \
         *                n30         \
         *               /   \         \
         *              /     \         \
         *             /       \         \
         *            /         \         \
         *           /           \         \
         *         n20            n21       \
         *         / \           /   \       \
         *        /   \         /     \       \
         *     n10     n11     n12     n13    n14
         *     / \     / \    /  \    /  \    /  \
         *   n00 n01 n02 n03 n04 n05 n06 n07 n08 n09
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n08 = Hash::digest("n08");
        auto n09 = Hash::digest("n09");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n14 = Hash::digest(n08, n09);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n30 = Hash::digest(n20, n21);
        auto n40 = Hash::digest(n30, n14);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        m.append_and_sign(n07, priv_key);
        m.append_and_sign(n08, priv_key);
        auto s = m.append_and_sign(n09, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n40));
    }

    {
        /*
         *                        n40
         *                       /    \
         *                      /      \
         *                     /        \
         *                    /          \
         *                   /            \
         *                  /              \
         *                n30               \
         *               /   \               \
         *              /     \               \
         *             /       \               \
         *            /         \               \
         *           /           \               \
         *         n20            n21            n22
         *         / \           /   \           / \
         *        /   \         /     \         /   \
         *     n10     n11     n12     n13    n14    \
         *     / \     / \    /  \    /  \    /  \    \
         *   n00 n01 n02 n03 n04 n05 n06 n07 n08 n09  n0a
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n08 = Hash::digest("n08");
        auto n09 = Hash::digest("n09");
        auto n0a = Hash::digest("n0a");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n14 = Hash::digest(n08, n09);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n22 = Hash::digest(n14, n0a);
        auto n30 = Hash::digest(n20, n21);
        auto n40 = Hash::digest(n30, n22);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        m.append_and_sign(n07, priv_key);
        m.append_and_sign(n08, priv_key);
        m.append_and_sign(n09, priv_key);
        auto s = m.append_and_sign(n0a, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n40));
    }

    {
        /*
         *                         n40
         *                        /   \
         *                       /     \
         *                      /       \
         *                     /         \
         *                    /           \
         *                   /             \
         *                  /               \
         *                n30                \
         *               /   \                \
         *              /     \                \
         *             /       \                \
         *            /         \                \
         *           /           \                \
         *         n20            n21             n22
         *         / \           /   \           /   \
         *        /   \         /     \         /     \
         *     n10     n11     n12     n13    n14      n15
         *     / \     / \    /  \    /  \    /  \     / \
         *   n00 n01 n02 n03 n04 n05 n06 n07 n08 n09 n0a n0b
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n08 = Hash::digest("n08");
        auto n09 = Hash::digest("n09");
        auto n0a = Hash::digest("n0a");
        auto n0b = Hash::digest("n0b");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n14 = Hash::digest(n08, n09);
        auto n15 = Hash::digest(n0a, n0b);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n22 = Hash::digest(n14, n15);
        auto n30 = Hash::digest(n20, n21);
        auto n40 = Hash::digest(n30, n22);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        m.append_and_sign(n07, priv_key);
        m.append_and_sign(n08, priv_key);
        m.append_and_sign(n09, priv_key);
        m.append_and_sign(n0a, priv_key);
        auto s = m.append_and_sign(n0b, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n40));
    }

    {
        /*
         *                              n40
         *                             /    \
         *                            /      \
         *                           /        \
         *                          /          \
         *                         /            \
         *                        /              \
         *                       /                \
         *                      /                  \
         *                     /                    \
         *                    /                      \
         *                   /                        \
         *                  /                          \
         *                n30                           n31
         *               /   \                         /   \
         *              /     \                       /     \
         *             /       \                     /       \
         *            /         \                   /         \
         *           /           \                 /           \
         *         n20            n21             n22           \
         *         / \           /   \           /   \           \
         *        /   \         /     \         /     \           \
         *     n10     n11     n12     n13    n14      n15         \
         *     / \     / \    /  \    /  \    /  \     / \          \
         *   n00 n01 n02 n03 n04 n05 n06 n07 n08 n09 n0a n0b        n0c
         */
        M m;

        auto n00 = Hash::digest("n00");
        auto n01 = Hash::digest("n01");
        auto n02 = Hash::digest("n02");
        auto n03 = Hash::digest("n03");
        auto n04 = Hash::digest("n04");
        auto n05 = Hash::digest("n05");
        auto n06 = Hash::digest("n06");
        auto n07 = Hash::digest("n07");
        auto n08 = Hash::digest("n08");
        auto n09 = Hash::digest("n09");
        auto n0a = Hash::digest("n0a");
        auto n0b = Hash::digest("n0b");
        auto n0c = Hash::digest("n0c");
        auto n10 = Hash::digest(n00, n01);
        auto n11 = Hash::digest(n02, n03);
        auto n12 = Hash::digest(n04, n05);
        auto n13 = Hash::digest(n06, n07);
        auto n14 = Hash::digest(n08, n09);
        auto n15 = Hash::digest(n0a, n0b);
        auto n20 = Hash::digest(n10, n11);
        auto n21 = Hash::digest(n12, n13);
        auto n22 = Hash::digest(n14, n15);
        auto n30 = Hash::digest(n20, n21);
        auto n31 = Hash::digest(n22, n0c);
        auto n40 = Hash::digest(n30, n31);

        m.append_and_sign(n00, priv_key);
        m.append_and_sign(n01, priv_key);
        m.append_and_sign(n02, priv_key);
        m.append_and_sign(n03, priv_key);
        m.append_and_sign(n04, priv_key);
        m.append_and_sign(n05, priv_key);
        m.append_and_sign(n06, priv_key);
        m.append_and_sign(n07, priv_key);
        m.append_and_sign(n08, priv_key);
        m.append_and_sign(n09, priv_key);
        m.append_and_sign(n0a, priv_key);
        m.append_and_sign(n0b, priv_key);
        auto s = m.append_and_sign(n0c, priv_key);

        BOOST_REQUIRE_EQUAL(s, priv_key.sign(n40));
    }
}

BOOST_AUTO_TEST_SUITE_END()

