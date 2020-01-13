#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "util/bytes.h"
#include "defer.h"
#include "client_config.h"
#include "version.h"
#include "upnp.h"

#include "cache/bep5_http/client.h"

#include <boost/asio/ip/address.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <network/uri.hpp>
#include <nlohmann/json.hpp>


using namespace std;
using namespace ouinet;
using json = nlohmann::json;

using Request = ClientFrontEnd::Request;
using Response = ClientFrontEnd::Response;

static string now_as_string() {
    namespace pt = boost::posix_time;
    auto entry_ts = pt::microsec_clock::universal_time();
    return pt::to_iso_extended_string(entry_ts);
}

struct ToggleInput {
    beast::string_view text;
    beast::string_view name;
    char shortcut;
    bool current_value;
};

template<typename E>
struct ClientFrontEnd::Input {
    string text;
    string name;
    vector<E> values;
    E current_value;

    Input(string text, string name, vector<E> values, E current_value)
        : text(move(text))
        , name(move(name))
        , values(move(values))
        , current_value(current_value)
    {}

    // Return true on change
    bool update(beast::string_view s) {
        auto i = s.find("?");
        if (i == beast::string_view::npos) return false;
        s = s.substr(i+1);
        if (s.substr(0, name.size()) != name) return false;
        s = s.substr(name.size());
        if (s.empty() || s[0] != '=') return false;
        s = s.substr(1);
        for (auto v : values) {
            stringstream ss;
            ss << v;
            if (ss.str() == s) {
                E prev = current_value;
                current_value = v;
                return prev != current_value;
            }
        }
        return false;
    }
};

namespace ouinet { // Need namespace here for argument-dependent-lookups to work

ostream& operator<<(ostream& os, const ToggleInput& i) {
    auto cur_value  = i.current_value ? "enabled" : "disabled";
    auto next_value = i.current_value ? "disable" : "enable";

    return os <<
          "<form method=\"get\">\n"
          "    " << i.text << ": " << cur_value << "&nbsp;"
                    "<input type=\"submit\" "
                           "name=\""  << i.name << "\" "
                           "accesskey=\""  << i.shortcut << "\" "
                           "value=\"" << next_value << "\"/>\n"
          "</form>\n";
}

template<typename E>
ostream& operator<<(ostream& os, const ClientFrontEnd::Input<E>& i) {
    os << "<form method=\"get\">\n"
          "    " << i.text << ": " << i.current_value << "&nbsp;"
          "        <select onchange=\"this.form.submit()\" name=\"" << i.name << "\">";

    for (auto e : i.values) {
        const char* selected = (e == i.current_value) ? "selected" : "";
        os << "<option value=\"" << e << "\" " << selected << ">" << e << "</option>";
    }

    os << "        </select>"
          "</form>\n";

    return os;
}

static ostream& operator<<(ostream& os, const std::chrono::steady_clock::duration& d) {
    using namespace chrono;

    unsigned int secs = duration_cast<seconds>(d).count();

    unsigned int hours   = secs / (60*60);   secs -= hours*60*60;
    unsigned int minutes = secs / 60;        secs -= minutes*60;

    if (hours)   { os << hours   << "h"; }
    if (minutes) { os << minutes << "m"; }

    return os << secs << "s";
}

static ostream& operator<<(ostream& os, const ClientFrontEnd::Task& task) {

    return os << task.id() << "| " << task.duration() << " | " << task.name();
}

} // ouinet namespace

ClientFrontEnd::ClientFrontEnd()
    : _log_level_input(new Input<log_level_t>("Log level", "loglevel", { SILLY, DEBUG, VERBOSE, INFO, WARN, ERROR, ABORT }, logger.get_threshold()))
    , _bep5_log_level_input(new Input<log_level_t>("Bep5Http log level", "bep5_loglevel", { SILLY, DEBUG, VERBOSE, INFO, WARN, ERROR, ABORT }, INFO))
{}

void ClientFrontEnd::handle_ca_pem( const Request& req, Response& res, stringstream& ss
                                  , const CACertificate& ca)
{
    res.set(http::field::content_type, "application/x-x509-ca-cert");
    res.set(http::field::content_disposition, "inline");

    ss << ca.pem_certificate();
}

void ClientFrontEnd::handle_portal( ClientConfig& config
                                  , const Request& req, Response& res, stringstream& ss
                                  , cache::bep5_http::Client* bep5_cache)
{
    res.set(http::field::content_type, "text/html");

    auto target = req.target();

    if (_log_level_input->update(target)) {
        logger.set_threshold(_log_level_input->current_value);
    }

    if (bep5_cache) {
        _bep5_log_level_input->current_value = bep5_cache->get_log_level();
        if (_bep5_log_level_input->update(target)) {
            bep5_cache->set_log_level(_bep5_log_level_input->current_value);
        }
    }

    if (target.find('?') != string::npos) {
        // XXX: Extra primitive value parsing.
        if (target.find("?origin_access=enable") != string::npos) {
            config.is_origin_access_enabled(true);
        }
        else if (target.find("?origin_access=disable") != string::npos) {
            config.is_origin_access_enabled(false);
        }
        else if (target.find("?proxy_access=enable") != string::npos) {
            config.is_proxy_access_enabled(true);
        }
        else if (target.find("?proxy_access=disable") != string::npos) {
            config.is_proxy_access_enabled(false);
        }
        else if (target.find("?injector_access=enable") != string::npos) {
            config.is_injector_access_enabled(true);
        }
        else if (target.find("?injector_access=disable") != string::npos) {
            config.is_injector_access_enabled(false);
        }
        else if (target.find("?auto_refresh=enable") != string::npos) {
            _auto_refresh_enabled = true;
        }
        else if (target.find("?auto_refresh=disable") != string::npos) {
            _auto_refresh_enabled = false;
        }
        else if (target.find("?distributed_cache=enable") != string::npos) {
            config.is_cache_access_enabled(true);
        }
        else if (target.find("?distributed_cache=disable") != string::npos) {
            config.is_cache_access_enabled(false);
        }

        // Redirect back to the portal.
        ss << "<!DOCTYPE html>\n"
               "<html>\n"
               "    <head>\n"
               "        <meta http-equiv=\"refresh\" content=\"0; url=./\"/>\n"
               "    </head>\n"
               "</html>\n";
        return;
    }

    ss << "<!DOCTYPE html>\n"
          "<html>\n"
          "    <head>\n";
    if (_auto_refresh_enabled) {
        ss << "      <meta http-equiv=\"refresh\" content=\"5\"/>\n";
    }
    ss << "      <style>\n"
          "        * {\n"
          "            font-family: \"Courier New\";\n"
          "            font-size: 10pt; }\n"
          "          }\n"
          "      </style>\n"
          "    </head>\n"
          "    <body>\n";

    // TODO: Do some browsers require P12 instead of PEM?
    ss << "      <p><a href=\"ca.pem\">Install client-specific CA certificate for HTTPS support</a>.\n"
          "      This certificate will only be used by your Ouinet-enabled applications in this device.\n"
          "      Verification of HTTPS content coming from the cache will be performed by injectors or publishers\n"
          "      that you have configured your Ouinet client to trust.\n"
          "      Verification of HTTPS content coming from the origin will be performed by your Ouinet client\n"
          "      using system-accepted Certification Authorities.</p>\n";

    ss << ToggleInput{"<u>A</u>uto refresh",   "auto_refresh",   'a', _auto_refresh_enabled};
    ss << ToggleInput{"<u>O</u>rigin access",  "origin_access",  'o', config.is_origin_access_enabled()};
    ss << ToggleInput{"<u>P</u>roxy access",   "proxy_access",   'p', config.is_proxy_access_enabled()};
    ss << ToggleInput{"<u>I</u>njector proxy", "injector_access",'i', config.is_injector_access_enabled()};
    ss << ToggleInput{"Distributed <u>C</u>ache", "distributed_cache",  'c', config.is_cache_access_enabled()};

    ss << *_log_level_input;

    ss << "<br>\n";
    ss << "Now: " << now_as_string()  << "<br>\n";
    ss << "Injector endpoint: " << config.injector_endpoint() << "<br>\n";

    if (_show_pending_tasks) {
        ss << "        <h2>Pending tasks " << _pending_tasks.size() << "</h2>\n";
        ss << "        <ul>\n";
        for (auto& task : _pending_tasks) {
            ss << "            <li><pre>" << task << "</pre></li>\n";
        }
        ss << "        </ul>\n";
    }

    if (bep5_cache) {
        ss << *_bep5_log_level_input;
    }

    ss << "    </body>\n"
          "</html>\n";
}

template<class Proto>
static
boost::optional<asio::ip::udp::endpoint>
local_endpoint(Proto proto, uint16_t port) {
    using namespace asio::ip;
    asio::io_context ctx;
    udp::socket s(ctx, proto);
    sys::error_code ec;
    if (proto == udp::v4()) {
        s.connect(udp::endpoint(make_address_v4("192.0.2.1"), 1234), ec);
    } else {
        s.connect(udp::endpoint(make_address_v6("2001:db8::1"), 1234), ec);
    }
    if (ec) return boost::none;
    return udp::endpoint(s.local_endpoint().address(), port);
}

void ClientFrontEnd::handle_status( ClientConfig& config
                                  , boost::optional<uint32_t> udp_port
                                  , const UPnPs& upnps
                                  , const Request& req, Response& res, stringstream& ss)
{
    res.set(http::field::content_type, "application/json");

    json response = {
        {"auto_refresh", _auto_refresh_enabled},
        {"origin_access", config.is_origin_access_enabled()},
        {"proxy_access", config.is_proxy_access_enabled()},
        {"injector_access", config.is_injector_access_enabled()},
        {"distributed_cache", config.is_cache_access_enabled()},
        {"ouinet_version", Version::VERSION_NAME},
        {"ouinet_build_id", Version::BUILD_ID}
    };

    if (udp_port) {
        using namespace asio::ip;

        auto epv4 = local_endpoint(udp::v4(), *udp_port);
        auto epv6 = local_endpoint(udp::v6(), *udp_port);

        std::vector<std::string> eps;
        eps.reserve(2);

        if (epv4) eps.push_back(util::str(*epv4));
        if (epv6) eps.push_back(util::str(*epv6));

        response["local_udp_endpoints"] = std::move(eps);
    }

    if (!upnps.empty()) {
        bool enabled = false;
        for (auto& pair : upnps) {
            if (pair.second->mapping_is_active()) enabled = true;
        }
        response["is_upnp_active"] = enabled ? "enabled" : "inactive";
    } else {
        response["is_upnp_active"] = "disabled";
    }

    ss << response;
}

Response ClientFrontEnd::serve( ClientConfig& config
                              , const Request& req
                              , cache::bep5_http::Client* bep5_cache
                              , const CACertificate& ca
                              , boost::optional<uint32_t> udp_port
                              , const UPnPs& upnps
                              , Yield yield)
{
    Response res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.keep_alive(false);

    stringstream ss;

    util::url_match url;
    match_http_url(req.target(), url);

    auto path = !url.path.empty() ? url.path : req.target().to_string();

    if (path == "/ca.pem") {
        handle_ca_pem(req, res, ss, ca);
    } else if (path == "/api/status") {
        handle_status(config, udp_port, upnps, req, res, ss);
    } else {
        handle_portal(config, req, res, ss, bep5_cache);
    }

    Response::body_type::reader reader(res, res.body());
    sys::error_code ec;
    reader.put(asio::buffer(ss.str()), ec);
    assert(!ec);

    res.prepare_payload();

    return res;
}

ClientFrontEnd::~ClientFrontEnd() {}

