#include "client_front_end.h"
#include "generic_stream.h"
#include "cache/cache_client.h"
#include "cache/btree.h"
#include "util.h"
#include "defer.h"
#include "client_config.h"
#include <boost/optional/optional_io.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <network/uri.hpp>
#include <json.hpp>


using namespace std;
using namespace ouinet;
using json = nlohmann::json;

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

void ClientFrontEnd::handle_ca_pem( const Request& req, Response& res, stringstream& ss
                                  , const CACertificate& ca)
{
    res.set(http::field::content_type, "application/x-x509-ca-cert");
    res.set(http::field::content_disposition, "inline");

    ss << ca.pem_certificate();
}

void ClientFrontEnd::handle_upload( const Request& req, Response& res, stringstream& ss
                                  , CacheClient* cache_client, asio::yield_context yield)
{
    static const string req_ctype = "application/octet-stream";

    auto result = http::status::ok;
    res.set(http::field::content_type, "application/json");
    string err, cid;

    if (req.method() != http::verb::post) {
        result = http::status::method_not_allowed;
        err = "request method is not POST";
    } else if (req[http::field::content_type] != req_ctype) {
        result = http::status::unsupported_media_type;
        err = "request content type is not " + req_ctype;
    } else if (!req[http::field::expect].empty()) {
        // TODO: Support ``Expect: 100-continue`` as cURL does,
        // e.g. to spot too big files before receiving the body.
        result = http::status::expectation_failed;
        err = "sorry, request expectations are not supported";
    } else if (!cache_client || !_ipfs_cache_enabled) {
        result = http::status::service_unavailable;
        err = "cache access is not available";
    } else {  // perform the upload
        sys::error_code ec;
        cid = cache_client->ipfs_add(req.body(), yield[ec]);
        if (ec) {
            result = http::status::internal_server_error;
            err = "failed to seed data to the cache";
        }
    }

    res.result(result);
    if (err.empty())
        ss << "{\"data_links\": [\"ipfs:/ipfs/" << cid << "\"]}";
    else
        ss << "{\"error\": \"" << err << "\"}";
}

static bool percent_decode(const string& in, string& out) {
    try {
        network::uri::decode(begin(in), end(in), back_inserter(out));
    } catch (const network::percent_decoding_error&) {
        return false;
    }
    return true;
}

static string percent_encode_all(const string& in) {
    // The URI library interface for doing this is really cumbersome
    // and we do not need a minimal or canonical encoding,
    // just something to allow passing the URI as a query argument
    // and avoid HTML inlining issues.
    // So we just encode everything but unreserved characters.
    stringstream outss;
    for (auto c : in)
        // Taken from <https://en.wikipedia.org/wiki/Percent-encoding#Types_of_URI_characters>.
        if ( (('0' <= c) && (c <= '9'))
             || (('A' <= c) && (c <= 'Z'))
             || (('a' <= c) && (c <= 'z'))
             || (c == '-') || (c == '_') || (c == '.') || (c == '~') )
            outss << c;
        else
            outss << boost::format("%%%02X") % static_cast<int>(c);
    return outss.str();
}

void ClientFrontEnd::handle_enumerate_index( const Request& req
                                           , Response& res
                                           , stringstream& ss
                                           , CacheClient* cache_client
                                           , asio::yield_context yield)
{
    res.set(http::field::content_type, "text/html");

    ss << "<!DOCTYPE html>\n"
           "<html>\n"
           "</html>\n"
           "<body style=\"font-family:monospace;white-space:nowrap;font-size:small\">\n";

    auto on_exit = defer([&] { ss << "</body></html>\n"; });

    if (!cache_client) {
        ss << "Cache is not initialized";
        return;
    }

    auto btree = cache_client->get_btree();

    if (!cache_client) {
        ss << "Cache does not sport BTree";
        return;
    }

    ss << "Index CID: " << btree->root_hash() << "<br/>\n";

    sys::error_code ec;
    Cancel cancel; // TODO: This should come from above
    auto iter = btree->begin(cancel, yield[ec]);

    if (ec) {
        ss << "Failed to retrieve BTree iterator: " << ec.message();
        return;
    }

    while (!iter.is_end()) {
        ss << "<a href=\"/api/descriptor?uri=" << percent_encode_all(iter.key()) << "\">"
           << iter.key() << "</a><br/>\n";

        iter.advance(cancel, yield[ec]);

        if (ec) {
            ss << "Failed enumerate the entire BTree: " << ec.message();
            return;
        }
    }
}

void ClientFrontEnd::handle_descriptor( const ClientConfig& config
                                      , const Request& req, Response& res, stringstream& ss
                                      , CacheClient* cache_client, asio::yield_context yield)
{
    auto result = http::status::ok;
    res.set(http::field::content_type, "application/json");
    string err;

    static const boost::regex uriqarx("[\\?&]uri=([^&]*)");
    boost::smatch urimatch;  // contains percent-encoded URI
    string uri;  // after percent-decoding
    auto target = req.target().to_string();  // copy to preserve regex result

    string file_descriptor;

    if (req.method() != http::verb::get) {
        result = http::status::method_not_allowed;
        err = "request method is not GET";
    } else if (!boost::regex_search(target, urimatch, uriqarx)) {
        result = http::status::bad_request;
        err = "missing \"uri\" query argument";
    } else if (!percent_decode(urimatch[1], uri)) {
        result = http::status::bad_request;
        err = "illegal encoding of URI argument";
    } else if (!cache_client || !_ipfs_cache_enabled) {
        result = http::status::service_unavailable;
        err = "cache access is not available";
    } else {  // perform the query
        sys::error_code ec;

        Cancel cancel; // TODO: This should come from above
        file_descriptor = cache_client->get_descriptor( key_from_http_url(uri)
                                                      , config.cache_index_type()
                                                      , cancel
                                                      , yield[ec]);

        if (ec == asio::error::not_found) {
            result = http::status::not_found;
            err = "URI was not found in the cache";
        } else if (ec) {
            result = http::status::internal_server_error;
            err = "failed to look up URI descriptor in the cache";
        }
    }

    res.result(result);

    if (err.empty())
        ss << file_descriptor;
    else
        ss << "{\"error\": \"" << err << "\"}";
}

void ClientFrontEnd::handle_insert_bep44( const Request& req, Response& res, stringstream& ss
                                        , CacheClient* cache_client, asio::yield_context yield)
{
    static const string req_ctype = "application/x-bittorrent";

    auto result = http::status::ok;
    res.set(http::field::content_type, "application/json");
    string err, key;

    if (req.method() != http::verb::post) {
        result = http::status::method_not_allowed;
        err = "request method is not POST";
    } else if (req[http::field::content_type] != req_ctype) {
        result = http::status::unsupported_media_type;
        err = "request content type is not " + req_ctype;
    } else if (!req[http::field::expect].empty()) {
        // TODO: Support ``Expect: 100-continue`` as cURL does,
        // e.g. to spot too big files before receiving the body.
        result = http::status::expectation_failed;
        err = "sorry, request expectations are not supported";
    } else {  // perform the insertion
        sys::error_code ec;

        Cancel cancel;
        key = cache_client->insert_mapping( req.body()
                                          , IndexType::bep44
                                          , cancel
                                          , yield[ec]);

        if (ec == asio::error::operation_not_supported) {
            result = http::status::service_unavailable;
            err = "BEP44 index is not enabled";
        } else if (ec == asio::error::invalid_argument) {
            result = http::status::unprocessable_entity;
            err = "malformed, incomplete or forged insertion data";
        } else if (ec) {
            result = http::status::internal_server_error;
            err = "failed to insert entry in index";
        }
    }

    res.result(result);
    if (err.empty())
        ss << "{\"key\": \"" << key << "\"}";
    else
        ss << "{\"error\": \"" << err << "\"}";
}

void ClientFrontEnd::handle_portal( ClientConfig& config
                                  , const Request& req, Response& res, stringstream& ss
                                  , CacheClient* cache_client)
{
    res.set(http::field::content_type, "text/html");

    auto target = req.target();

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
        else if (target.find("?injector_proxy=enable") != string::npos) {
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

    ss << ToggleInput{"Auto refresh",   "auto_refresh",   _auto_refresh_enabled};
    ss << ToggleInput{"Origin access", "origin_access", config.is_origin_access_enabled()};
    ss << ToggleInput{"Proxy access", "proxy_access", config.is_proxy_access_enabled()};
    ss << ToggleInput{"Injector proxy", "injector_proxy", _injector_proxying_enabled};
    ss << ToggleInput{"IPFS Cache",     "ipfs_cache",     _ipfs_cache_enabled};

    ss << "<br>\n";
    ss << "<form action=\"/api/descriptor\" method=\"get\">\n"
       << "    Query URI descriptor: <input name=\"uri\"/ placeholder=\"URI\" size=\"100\">\n"
       << "    <input type=\"submit\" value=\"Submit\"/>\n"
       << "</form>\n";

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

    if (cache_client) {
        ss << "        Our IPFS ID (IPNS): " << cache_client->ipfs_id() << "<br>\n";
        ss << "        <h2>Index</h2>\n";
        ss << "        IPNS: " << cache_client->ipns() << "<br>\n";
        ss << "        IPFS: <a href=\"index.html\">" << cache_client->ipfs() << "</a><br>\n";
    }

    ss << "    </body>\n"
          "</html>\n";
}

void ClientFrontEnd::handle_status( ClientConfig& config
                                  , const Request& req, Response& res, stringstream& ss
                                  , CacheClient* cache_client)
{
    res.set(http::field::content_type, "application/json");

    json response = {
        {"auto_refresh", _auto_refresh_enabled},
        {"origin_access", config.is_origin_access_enabled()},
        {"proxy_access", config.is_proxy_access_enabled()},
        {"injector_proxy", _injector_proxying_enabled},
        {"ipfs_cache", _ipfs_cache_enabled},
     // https://github.com/nlohmann/json#arbitrary-types-conversions
     // {"misc", {
         // {"injector_endpoint", config.injector_endpoint()},
         // {"pending_tasks", _pending_tasks},
         // {"our_ipfs_id", cache_client->ipfs_id()},
         // {"cache_ipns_id", cache_client->ipns()},
         // {"cache_ipns_id", cache_client->ipfs()}
     // }}
    };

    ss << response;
}

Response ClientFrontEnd::serve( ClientConfig& config
                              , const Request& req
                              , CacheClient* cache_client
                              , const CACertificate& ca
                              , asio::yield_context yield)
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
    } else if (path == "/index.html") {
        sys::error_code ec_;  // shouldn't throw, but just in case
        handle_enumerate_index(req, res, ss, cache_client, yield[ec_]);
    } else if (path == "/api/upload") {
        sys::error_code ec_;  // shouldn't throw, but just in case
        handle_upload(req, res, ss, cache_client, yield[ec_]);
    } else if (path == "/api/descriptor") {
        sys::error_code ec_;  // shouldn't throw, but just in case
        handle_descriptor(config, req, res, ss, cache_client, yield[ec_]);
    } else if (path == "/api/insert/bep44") {
        sys::error_code ec_;  // shouldn't throw, but just in case
        handle_insert_bep44(req, res, ss, cache_client, yield[ec_]);
    } else if (path == "/api/status") {
        handle_status(config, req, res, ss, cache_client);
    } else {
        handle_portal(config, req, res, ss, cache_client);
    }

    Response::body_type::reader reader(res, res.body());
    sys::error_code ec;
    reader.put(asio::buffer(ss.str()), ec);
    assert(!ec);

    res.prepare_payload();

    return res;
}

