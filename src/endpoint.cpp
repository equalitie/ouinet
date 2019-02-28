#include "endpoint.h"

namespace ouinet {

boost::optional<Endpoint> parse_endpoint(beast::string_view endpoint)
{
    size_t pos = endpoint.find(':');
    if (pos == std::string::npos) {
        return boost::none;
    }
    beast::string_view type = endpoint.substr(0, pos);
    Endpoint output;
    output.endpoint_string = endpoint.substr(pos + 1).to_string();

    if (type == "tcp") {
        output.type = Endpoint::TcpEndpoint;
    } else if (type == "i2p") {
        output.type = Endpoint::I2pEndpoint;
#ifdef USE_GNUNET
    } else if (type == "gnunet") {
        output.type = Endpoint::GnunetEndpoint;
#endif
    } else if (type == "obfs2") {
        output.type = Endpoint::Obfs2Endpoint;
    } else if (type == "obfs3") {
        output.type = Endpoint::Obfs3Endpoint;
    } else if (type == "obfs4") {
        output.type = Endpoint::Obfs4Endpoint;
    } else {
        return boost::none;
    }
    return output;
}

std::ostream& operator<<(std::ostream& os, const Endpoint& ep)
{
    if (ep.type == Endpoint::TcpEndpoint) {
        os << "tcp";
    } else if (ep.type == Endpoint::I2pEndpoint) {
        os << "i2p";
#ifdef USE_GNUNET
    } else if (ep.type == Endpoint::GnunetEndpoint) {
        os << "gnunet";
#endif
    } else if (ep.type == Endpoint::Obfs2Endpoint) {
        os << "obfs2";
    } else if (ep.type == Endpoint::Obfs3Endpoint) {
        os << "obfs3";
    } else if (ep.type == Endpoint::Obfs4Endpoint) {
        os << "obfs4";
    } else {
        assert(false);
    }

    os << ":";
    os << ep.endpoint_string;

    return os;
}

} // ouinet namespace
