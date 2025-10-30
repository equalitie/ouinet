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
    output.endpoint_string = std::string(endpoint.substr(pos + 1));

    if (type == "tcp") {
        output.type = Endpoint::TcpEndpoint;
    } else if (type == "utp") {
        output.type = Endpoint::UtpEndpoint;
#ifdef USE_GNUNET
    } else if (type == "gnunet") {
        output.type = Endpoint::GnunetEndpoint;
#endif
#ifdef __EXPERIMENTAL__
    } else if (type == "i2p") {
        output.type = Endpoint::I2pEndpoint;
#endif // ifdef __EXPERIMENTAL__
#ifdef __DEPRECATED__
    } else if (type == "lampshade") {
        output.type = Endpoint::LampshadeEndpoint;
    } else if (type == "obfs2") {
        output.type = Endpoint::Obfs2Endpoint;
    } else if (type == "obfs3") {
        output.type = Endpoint::Obfs3Endpoint;
    } else if (type == "obfs4") {
        output.type = Endpoint::Obfs4Endpoint;
#endif // ifdef  __DEPRECATED__
    } else if (type == "bep5") {
        output.type = Endpoint::Bep5Endpoint;
    } else {
        return boost::none;
    }
    return output;
}

std::ostream& operator<<(std::ostream& os, const Endpoint& ep)
{
    if (ep.type == Endpoint::TcpEndpoint) {
        os << "tcp";
    } else if (ep.type == Endpoint::UtpEndpoint) {
        os << "utp";
#ifdef USE_GNUNET
    } else if (ep.type == Endpoint::GnunetEndpoint) {
        os << "gnunet";
#endif
#ifdef __EXPERIMENTAL__
    } else if (ep.type == Endpoint::I2pEndpoint) {
        os << "i2p";
#endif // ifdef __EXPERIMENTAL__
#ifdef __DEPRECATED__
    } else if (ep.type == Endpoint::LampshadeEndpoint) {
        os << "lampshade";
    } else if (ep.type == Endpoint::Obfs2Endpoint) {
        os << "obfs2";
    } else if (ep.type == Endpoint::Obfs3Endpoint) {
        os << "obfs3";
    } else if (ep.type == Endpoint::Obfs4Endpoint) {
        os << "obfs4";
#endif // ifdef __DEPRECATED__
    } else if (ep.type == Endpoint::Bep5Endpoint) {
        os << "bep5";
    } else {
        assert(false);
    }

    os << ":";
    os << ep.endpoint_string;

    return os;
}

} // ouinet namespace
