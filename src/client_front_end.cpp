#include "client_front_end.h"
#include "generic_stream.h"
#include "cache/bep44_ipfs/cache_client.h"
#include "util.h"
#include "util/bytes.h"
#include "defer.h"
#include "client_config.h"
#include "version.h"

// For parsing BEP44 insertion data.
#include "bittorrent/mutable_data.h"
#include "cache/bep44_ipfs/descidx.h"
#include "cache/bep44_ipfs/http_desc.h"

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
{}

void ClientFrontEnd::handle_ca_pem( const Request& req, Response& res, stringstream& ss
                                  , const CACertificate& ca)
{
    res.set(http::field::content_type, "application/x-x509-ca-cert");
    res.set(http::field::content_disposition, "inline");

    ss << ca.pem_certificate();
}

void ClientFrontEnd::handle_upload( const ClientConfig& config
                                  , const Request& req, Response& res, stringstream& ss
                                  , bep44_ipfs::CacheClient* cache_client
                                  , asio::yield_context yield)
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
    } else if (!cache_client || !config.is_cache_access_enabled()) {
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

void ClientFrontEnd::handle_descriptor( const ClientConfig& config
                                      , const Request& req, Response& res, stringstream& ss
                                      , bep44_ipfs::CacheClient* cache_client, Yield yield)
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
    } else if (!cache_client || !config.is_cache_access_enabled()) {
        result = http::status::service_unavailable;
        err = "cache access is not available";
    } else {  // perform the query
        sys::error_code ec;

        Cancel cancel; // TODO: This should come from above
        file_descriptor = cache_client->get_descriptor( key_from_http_url(uri)
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

// Extract key from BEP44 insertion data containing an inlined descriptor
// (linked descriptors are not yet supported).
static
string key_from_bep44(const string& data, Cancel& cancel, asio::yield_context yield)
{
    string key;
    sys::error_code ec;
    try {
        auto item = bittorrent::MutableDataItem::bdecode(data);  // opt<bep44/m>
        if (!item) throw invalid_argument("");

        auto desc_path = item->value.as_string();  // opt<path to serialized desc>
        if (!desc_path) throw invalid_argument("");

        static auto desc_load = [](auto, auto&, auto y) {  // TODO: support linked descriptors
                                    return or_throw(y, asio::error::operation_not_supported, "");
                                };
        auto desc_data = bep44_ipfs::descriptor::from_path( *desc_path, desc_load
                                                          , cancel, yield[ec]);  // serialized desc
        if (ec) throw invalid_argument("");

        auto desc = bep44_ipfs::Descriptor::deserialize(desc_data);  // opt<desc>
        if (!desc) throw invalid_argument("");

        key = key_from_http_url(desc->url);
    } catch (invalid_argument _) {
        ec = asio::error::invalid_argument;
    }
    return or_throw(yield, ec, move(key));
}

void ClientFrontEnd::handle_insert_bep44( const Request& req, Response& res, stringstream& ss
                                        , bep44_ipfs::CacheClient* cache_client
                                        , asio::yield_context yield)
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
        Cancel cancel; // TODO: This should come from above

        // `ClientIndex` does not know about descriptor format,
        // and `CacheClient` does not know about BEP44 format,
        // so the proper place to extract the key from insertion data is here.
        auto body = req.body();
        auto key = key_from_bep44(body, cancel, yield[ec]);
        if (!ec)
            key = cache_client->insert_mapping( key
                                              , move(body)
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
                                  , bep44_ipfs::CacheClient* cache_client)
{
    res.set(http::field::content_type, "text/html");

    auto target = req.target();

    if (_log_level_input->update(target)) {
        logger.set_threshold(_log_level_input->current_value);
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
        else if (target.find("?injector_proxy=enable") != string::npos) {
            config.is_injector_access_enabled(true);
        }
        else if (target.find("?injector_proxy=disable") != string::npos) {
            config.is_injector_access_enabled(false);
        }
        else if (target.find("?auto_refresh=enable") != string::npos) {
            _auto_refresh_enabled = true;
        }
        else if (target.find("?auto_refresh=disable") != string::npos) {
            _auto_refresh_enabled = false;
        }
        else if (target.find("?ipfs_cache=enable") != string::npos) {
            config.is_cache_access_enabled(true);
        }
        else if (target.find("?ipfs_cache=disable") != string::npos) {
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
    ss << ToggleInput{"<u>I</u>njector proxy", "injector_proxy", 'i', config.is_injector_access_enabled()};
    ss << ToggleInput{"Distributed <u>C</u>ache", "ipfs_cache",  'c', config.is_cache_access_enabled()};

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

    if (cache_client) {
        ss << "        Our IPFS ID (IPNS): " << cache_client->ipfs_id() << "<br>\n";
        ss << "        <h2>Index</h2>\n";
        ss << "        Please use the box below to query the descriptor of an arbitrary URI without fetching the associated content.<br>\n";

        ss << "        <br>\n";
        ss << "        <form action=\"/api/descriptor\" method=\"get\">\n"
           << "            Query URI descriptor: <input name=\"uri\"/ placeholder=\"URI\" size=\"100\">\n"
           << "        <input type=\"submit\" value=\"Submit\"/>\n"
           << "        </form>\n";

        auto bep44_pk = config.index_bep44_pub_key();
        auto bep44_pk_s = bep44_pk ? util::bytes::to_hex(bep44_pk->serialize()) : "";
        ss << "        <br>\n";
        ss << "        BEP44 public key: " << bep44_pk_s << "<br>\n";
    }

    ss << "    </body>\n"
          "</html>\n";
}

void ClientFrontEnd::handle_status( ClientConfig& config
                                  , const Request& req, Response& res, stringstream& ss
                                  , bep44_ipfs::CacheClient* cache_client)
{
    res.set(http::field::content_type, "application/json");

    json response = {
        {"auto_refresh", _auto_refresh_enabled},
        {"origin_access", config.is_origin_access_enabled()},
        {"proxy_access", config.is_proxy_access_enabled()},
        {"injector_proxy", config.is_injector_access_enabled()},
        {"ipfs_cache", config.is_cache_access_enabled()},
        {"ouinet_version", Version::VERSION_NAME},
        {"ouinet_build_id", Version::BUILD_ID}
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
                              , bep44_ipfs::CacheClient* cache_client
                              , const CACertificate& ca
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
    } else if (path == "/api/upload") {
        sys::error_code ec_;  // shouldn't throw, but just in case
        handle_upload(config, req, res, ss, cache_client, yield[ec_]);
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

ClientFrontEnd::~ClientFrontEnd() {}

