#include "endpoint.h"

namespace ouinet {

std::ostream& operator<<(std::ostream& os, const GnunetEndpoint& ep)
{
    return os << ep.host << ":" << ep.port;
}

std::ostream& operator<<(std::ostream& os, const Endpoint& ep)
{
    struct Visitor {
        std::ostream& os;

        void operator()(const asio::ip::tcp::endpoint& ep) {
            os << ep;
        }

        void operator()(const GnunetEndpoint& ep) {
            os << ep;
        }
    };

    Visitor visitor{os};
    boost::apply_visitor(visitor, ep);

    return os;
}

} // ouinet namespace
