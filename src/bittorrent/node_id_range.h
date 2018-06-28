#include "node_id.h"

namespace ouinet { namespace bittorrent {

class NodeIdRange {
public:
    NodeID stencil;
    size_t mask;

    NodeID random_id() const {
        return NodeID::random(stencil, mask);
    }

    NodeIdRange reduced(bool bit) {
        NodeIdRange ret{stencil, mask};
        ++ret.mask;
        ret.stencil.set_bit(ret.mask, bit);
        return ret;
    }

    static NodeIdRange max() {
        return { NodeID::zero(), 0 };
    }
};

}} // namespaces
