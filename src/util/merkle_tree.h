#pragma once

#include <vector>
#include <boost/optional.hpp>

namespace ouinet { namespace util {

template<class H, class C>
class MerkleTree {
public:
    using Hash       = H;
    using Crypto     = C;
    using Digest     = typename Hash::digest_type;
    using PrivateKey = typename Crypto::PrivateKey;
    using Signature  = typename PrivateKey::sig_array_t;

public:
    Signature append_and_sign(Digest, const PrivateKey&);

private:
    /*
     * Return the layer index where we'll add a new node that "connects" the
     * leaf node we're currently adding with an existing node one layer below.
     *
     * Input is the number of leaf nodes in the tree prior to adding the leaf
     * node.
     *
     * Example #1:
     *
     *     get_connect_layer(1 = num of *leaf* nodes in the input tree) -> 1
     *
     *                    <n10>
     *     n00    ->      /   \
     *                  n00   n01
     *
     *
     *     Here the result is 1, because the that's the index of the layer
     *     where we put n10.
     *
     * Example #2:
     *
     *     get_connect_layer(6 = num of *leaf* nodes in the input tree) -> 2
     *
     *                                              n30
     *                                             /    \
     *                                            /      \
     *            n30                            /        \
     *           /   \                          /          \
     *          /     \                        /            \
     *        n20      \                     n20           <n21>
     *        / \       \         ->         / \            / \
     *       /   \       \                  /   \          /   \
     *      /     \       \                /     \        /     \
     *    n10     n11     n13            n01     n11    n12      \
     *    / \     / \    /  \            / \     / \    /  \      \
     *  n00 n01 n02 n03 n04 n05        n00 n01 n02 n03 n04 n05    n06
     *
     */
    static size_t get_connect_layer(size_t n) {
        size_t l = 0;
        while (n) { ++l ; if (n&1) break; n >>= 1; }
        return l;
    };

private:
    template<class H_, class C_>
    friend std::ostream& operator<<(std::ostream&, const MerkleTree<H_,C_>&);

    // Nodes are sorted in layers. _nodes[0] is the "leaf layer", _nodes[1] is
    // the layer above,...
    std::vector<std::vector<Digest>> _layers;
};

template<class H, class C>
inline
typename MerkleTree<H,C>::Signature
MerkleTree<H,C>::append_and_sign(Digest d, const PrivateKey& priv_key) {
    size_t ci = get_connect_layer(_layers.empty() ? 0 : _layers[0].size());

    assert(ci <= _layers.size());

    if (ci == _layers.size()) {
        _layers.emplace_back();
    }

    size_t root_layer = _layers.size() - 1;
    size_t di = 0;

    _layers[di].push_back(d);

    if (ci != di) {
        _layers[ci].emplace_back();

        while (true) {
            auto i = std::prev(_layers[di].end());
            auto j = ci == di + 1
                   ? std::prev(i)
                   : std::prev(_layers[ci-1].end());

            _layers[ci].back() = Hash::digest(*j, *i);

            if (ci == root_layer) break;

            di = ci;
            ci += get_connect_layer(_layers[ci].size()-1);
        }
    }

    return priv_key.sign(_layers[root_layer][0]);
}

template<class H, class C>
inline
std::ostream& operator<<(std::ostream& os, const MerkleTree<H,C>& m)
{
    size_t li = 0;
    for (auto& l : m._layers) {
        os << li << ":";
        for (auto& n : l) {
            os << n.digest << ";";
        }
        os << "\n";
        ++li;
    }
    return os;
}

} // namespace util
} // namespace ouinet
