#include "endpoint.h"

namespace ouinet {

#ifdef USE_GNUNET
std::ostream& operator<<(std::ostream& os, const GnunetEndpoint& ep)
{
    return os << ep.host << ":" << ep.port;
}
#endif

std::ostream& operator<<(std::ostream& os, const I2PEndpoint& ep)
{
    return os << ep.pubkey;
}

std::ostream& operator<<(std::ostream& os, const Endpoint& ep)
{
    struct Visitor {
        std::ostream& os;

        void operator()(const asio::ip::tcp::endpoint& ep) {
            os << ep;
        }

#ifdef USE_GNUNET
        void operator()(const GnunetEndpoint& ep) {
            os << ep;
        }
#endif

        void operator()(const I2PEndpoint& ep) {
            os << ep;
        }
    };

    Visitor visitor{os};
    boost::apply_visitor(visitor, ep);

    return os;
}

} // ouinet namespace
