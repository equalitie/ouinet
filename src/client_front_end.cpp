#include "client_front_end.h"
#include "generic_connection.h"
#include "cache/cache_client.h"
#include <boost/optional/optional_io.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/regex.hpp>


using namespace std;
using namespace ouinet;

using Request = ClientFrontEnd::Request;
using Response = ClientFrontEnd::Response;
using boost::optional;

static string now_as_string() {
    namespace pt = boost::posix_time;
    auto entry_ts = pt::microsec_clock::universal_time();
    return pt::to_iso_extended_string(entry_ts);
}

struct ToggleInput {
    beast::string_view text;
    beast::string_view name;
    bool current_value;
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
                           "value=\"" << next_value << "\"/>\n"
          "</form>\n";
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

static
string get_url_path(const string url) {
    // This is not a bullet-proof URL parser, it just gets some common cases here.
    static const boost::regex urlrx("^(?:http://[-\\.a-z0-9]+)?(/[^?#]*).*");
    boost::smatch url_match;
    boost::regex_match(url, url_match, urlrx);
    return url_match[1];
}

void ClientFrontEnd::handle_ca_pem( const Request& req, Response& res, stringstream& ss
                                  , const CACertificate& ca)
{
    res.set(http::field::content_type, "application/x-x509-ca-cert");
    res.set(http::field::content_disposition, "inline");

    ss << ca.pem_certificate();
}

void ClientFrontEnd::handle_portal( const Request& req, Response& res, stringstream& ss
                                  , const boost::optional<Endpoint>& injector_ep
                                  , CacheClient* cache_client)
{
    res.set(http::field::content_type, "text/html");

    auto target = req.target();

    if (target.find('?') != string::npos) {
        // XXX: Extra primitive value parsing.
        if (target.find("?injector_proxy=enable") != string::npos) {
            _injector_proxying_enabled = true;
        }
        else if (target.find("?injector_proxy=disable") != string::npos) {
            _injector_proxying_enabled = false;
        }
        else if (target.find("?auto_refresh=enable") != string::npos) {
            _auto_refresh_enabled = true;
        }
        else if (target.find("?auto_refresh=disable") != string::npos) {
            _auto_refresh_enabled = false;
        }
        else if (target.find("?ipfs_cache=enable") != string::npos) {
            _ipfs_cache_enabled = true;
        }
        else if (target.find("?ipfs_cache=disable") != string::npos) {
            _ipfs_cache_enabled = false;
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
        ss << "      <meta http-equiv=\"refresh\" content=\"1\"/>\n";
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
    ss << "      <p><a href=\"ca.pem\">Install client-specific CA certificate for HTTPS support</a></p>\n";

    ss << ToggleInput{"Auto refresh",   "auto_refresh",   _auto_refresh_enabled};
    ss << ToggleInput{"Injector proxy", "injector_proxy", _injector_proxying_enabled};
    ss << ToggleInput{"IPFS Cache",     "ipfs_cache",     _ipfs_cache_enabled};

    ss << "<br>\n";
    ss << "Now: " << now_as_string()  << "<br>\n";
    ss << "Injector endpoint: " << injector_ep << "<br>\n";

    if (_show_pending_tasks) {
        ss << "        <h2>Pending tasks " << _pending_tasks.size() << "</h2>\n";
        ss << "        <ul>\n";
        for (auto& task : _pending_tasks) {
            ss << "            <li><pre>" << task << "</pre></li>\n";
        }
        ss << "        </ul>\n";
    }

    if (cache_client) {
        ss << "        Our IPFS ID (IPNS): " << cache_client->id() << "<br>\n";
        ss << "        <h2>Database</h2>\n";
        ss << "        IPNS: " << cache_client->ipns() << "<br>\n";
        ss << "        IPFS: " << cache_client->ipfs() << "<br>\n";
    }

    ss << "    </body>\n"
          "</html>\n";
}

Response ClientFrontEnd::serve( const boost::optional<Endpoint>& injector_ep
                              , const Request& req
                              , CacheClient* cache_client
                              , const CACertificate& ca)
{
    Response res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.keep_alive(false);

    stringstream ss;

    auto url_path = get_url_path(req.target().to_string());
    if (url_path == "/ca.pem")
        handle_ca_pem(req, res, ss, ca);
    else
        handle_portal(req, res, ss, injector_ep, cache_client);

    Response::body_type::writer writer(res);
    sys::error_code ec;
    writer.put(asio::buffer(ss.str()), ec);
    assert(!ec);

    res.prepare_payload();

    return res;
}

