#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "util/bytes.h"
#include "defer.h"
#include "client_config.h"
#include "version.h"
#include "upnp_updater.h"
#include "split_string.h"
#include "or_throw.h"

#include "bittorrent/dht.h"
#include "cache/client.h"
#include "ouiservice/bep5/client.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <memory>
#include <nlohmann/json.hpp>


using namespace std;
using namespace ouinet;
using json = nlohmann::json;

using Request = ClientFrontEnd::Request;
using Response = ClientFrontEnd::Response;

static string time_as_string(const boost::posix_time::ptime& t) {
    return boost::posix_time::to_iso_extended_string(t);
}

static string past_as_string(const boost::posix_time::time_duration& d) {
    auto past_ts = boost::posix_time::microsec_clock::universal_time() - d;
    return time_as_string(past_ts);
}

static string now_as_string() {
    auto entry_ts = boost::posix_time::microsec_clock::universal_time();
    return time_as_string(entry_ts);
}

// Escape an input string so that it can be safely embedded into HTML.
static string as_safe_html(string s) {
    boost::replace_all(s, "&", "&amp;");
    boost::replace_all(s, "<", "&lt;");
    boost::replace_all(s, "<", "&gt;");
    boost::replace_all(s, "\"", "&quot;");
    boost::replace_all(s, "'", "&#39;");
    return s;
}

template<typename E>
static string as_safe_html(E e) {
    return as_safe_html(util::str(e));
}

struct TextInput {
    beast::string_view html_label;
    char shortcut;
    beast::string_view name;
    beast::string_view placeholder;
    std::string current_value;
};

struct ToggleInput {
    beast::string_view html_label;
    char shortcut;
    beast::string_view name;
    bool current_value;
};

template<typename E>
struct ClientFrontEnd::Input {
    string html_label;
    char shortcut;
    string name;
    vector<E> values;
    E current_value;

    Input( string html_label, char shortcut, string name
         , vector<E> values, E current_value)
        : html_label(move(html_label))
        , shortcut(shortcut)
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
            ostringstream ss;
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

ostream& operator<<(ostream& os, const TextInput& i) {
    return os <<
          "<form method=\"get\">\n"
          "    <label>" << i.html_label << ": "
                    "<input type=\"text\" "
                           "name=\""  << i.name << "\" id=\"input-" << i.name << "\" "
                           "accesskey=\""  << i.shortcut << "\" "
                           "value=\"" << as_safe_html(i.current_value) << "\" "
                           "placeholder=\"" << as_safe_html(i.placeholder) << "\"/>"
                    "<input type=\"submit\" value=\"set\"/></label>\n"
          "</form>\n";
}

ostream& operator<<(ostream& os, const ToggleInput& i) {
    auto cur_value  = i.current_value ? "enabled" : "disabled";
    auto next_value = i.current_value ? "disable" : "enable";

    return os <<
          "<form method=\"get\">\n"
          "    <label>" << i.html_label << ": " << cur_value << "&nbsp;"
                    "<input type=\"submit\" "
                           "name=\""  << i.name << "\" id=\"input-" << i.name << "\" "
                           "accesskey=\""  << i.shortcut << "\" "
                           "value=\"" << next_value << "\"/></label>\n"
          "</form>\n";
}

template<typename E>
ostream& operator<<(ostream& os, const ClientFrontEnd::Input<E>& i) {
    os << "<form method=\"get\">\n"
          "    <label>" << i.html_label << ": " << as_safe_html(i.current_value) << "&nbsp;"
                    "<select onchange=\"this.form.submit()\" "
                            "name=\"" << i.name << "\" id=\"input-" << i.name << "\" "
                            "accesskey=\""  << i.shortcut << "\">";

    for (auto e : i.values) {
        const char* selected = (e == i.current_value) ? " selected" : "";
        os << "<option value=\"" << as_safe_html(e) << "\"" << selected << ">"
           << as_safe_html(e) << "</option>";
    }

    os << "</select></label>\n"
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

ClientFrontEnd::ClientFrontEnd(const ClientConfig& config)
    : _log_level_input(new Input<log_level_t>( "Log le<u>v</u>el", 'v', "loglevel"
                                             , { SILLY, DEBUG, VERBOSE, INFO, WARN, ERROR_LEVEL, ABORT }, config.log_level()))
{}

void ClientFrontEnd::handle_ca_pem( const Request& req, Response& res, ostringstream& ss
                                  , const CACertificate& ca)
{
    res.set(http::field::content_type, "application/x-x509-ca-cert");
    res.set(http::field::content_disposition, "inline");

    ss << ca.pem_certificate();
}

void ClientFrontEnd::enable_log_to_file(ClientConfig& config) {
    if (config.is_log_file_enabled()) return;

    _log_level_no_file = config.log_level();
    _log_level_input->current_value = DEBUG;
    config.log_level(DEBUG);
    config.is_log_file_enabled(true);
}

void ClientFrontEnd::disable_log_to_file(ClientConfig& config) {
    if (!config.is_log_file_enabled()) return;

    config.is_log_file_enabled(false);
    if (!_log_level_no_file)  // enabled in a previous run
        _log_level_no_file = default_log_level();
    config.log_level(*_log_level_no_file);
    _log_level_input->current_value = *_log_level_no_file;
}

static void load_log_file(ClientConfig& config, ostringstream& out_ss) {
    if (!config.is_log_file_enabled()) return;
    std::fstream* logfile = logger.get_log_file();
    assert(logfile && "No log file in spite of configuration saying so");
    logfile->flush();
    logfile->seekg(0);
    std::copy( istreambuf_iterator<char>(*logfile)
             , istreambuf_iterator<char>()
             , ostreambuf_iterator<char>(out_ss));
}

template<class EndPoint>
static
boost::optional<EndPoint>
local_endpoint(const EndPoint& local_ep) {
    using namespace asio::ip;
    boost::optional<address> local_addr;
    auto proto = local_ep.protocol();
    if (proto == udp::v4()) {
        if (local_ep.address() != address_v4::any()) return local_ep;  // explicit addr
        local_addr = util::get_local_ipv4_address();  // find source addr
    } else if (proto == udp::v6()) {
        if (local_ep.address() != address_v6::any()) return local_ep;  // explicit addr
        local_addr = util::get_local_ipv6_address();  // find source addr
    } else {
        assert(0 && "Invalid local UDP endpoint protocol");
    }
    if (!local_addr) return boost::none;
    return EndPoint(*local_addr, local_ep.port());
}

static
std::vector<std::string>
local_udp_endpoints(const ClientFrontEnd::UdpEndpoint& local_ep) {
    // This used to return both IPv4 and IPv6 endpoints,
    // but now we only return the actual endpoint
    // (still as a vector for backwards compatibility.
    auto ep = local_endpoint(local_ep);

    std::vector<std::string> eps;
    eps.reserve(1);

    if (ep) eps.push_back(util::str(*ep));

    return eps;
}

static
std::vector<std::string>
external_udp_endpoints(const ClientFrontEnd::UPnPs& upnps) {
    if (upnps.empty()) return {};

    std::vector<std::string> eps;
    for (auto& pair : upnps) {
        if (!pair.second) continue;
        for (auto& ep : pair.second->get_external_endpoints())
            eps.push_back(util::str(ep));
    }
    return eps;
}

static
std::string
upnp_status(const ClientFrontEnd::UPnPs& upnps) {
    if (upnps.empty()) return "disabled";

    bool available = false;
    for (auto& pair : upnps) {
        if (!pair.second) return "disabled";
        if (pair.second->mapping_is_active()) return "enabled";
        available = available || pair.second->is_available();
    }
    // A stable "inactive" value should be taken as a warning sign of
    // something not properly working with the UPnP setup.
    return available ? "inactive" : "disabled";
}

static
std::vector<std::string>
public_udp_endpoints(const bittorrent::DhtBase& dht) {
    std::vector<std::string> eps;
    for (auto& ep : dht.wan_endpoints())
        eps.push_back(util::str(ep));
    return eps;
}

static
std::vector<std::string>
bt_extra_bootstraps(const ClientConfig& config) {
    std::vector<std::string> bsx;
    for (auto& bs : config.bt_bootstrap_extras())
        bsx.push_back(util::str(bs));
    return bsx;
}

static
std::string
get_bt_extra_bootstraps(ClientConfig& config) {
    return boost::algorithm::join(bt_extra_bootstraps(config), " ");
}

static
bool
set_bt_extra_bootstraps(beast::string_view v, ClientConfig& config) {
    // Limit input length and undo form URL-encoded spaces.
    auto split = SplitString(v.substr(0, 256), '+');

    ClientConfig::ExtraBtBsServers bsx;
    for (const auto& bs_enc : split) {
        if (bs_enc.empty()) continue;
        auto bs = util::percent_decode(bs_enc);
        if (bs.empty()) return false;
        auto bs_addr = bittorrent::bootstrap::parse_address(bs);
        if (!bs_addr) return false;
        bsx.insert(*bs_addr);
    }
    config.bt_bootstrap_extras(std::move(bsx));
    return true;
}

static
std::string
reachability_status(const util::UdpServerReachabilityAnalysis& reachability) {
    switch (reachability.judgement()) {
    case util::UdpServerReachabilityAnalysis::Reachability::Undecided:
        return "undecided";
    case util::UdpServerReachabilityAnalysis::Reachability::ConfirmedReachable:
        return "reachable";
    case util::UdpServerReachabilityAnalysis::Reachability::UnconfirmedReachable:
        return "likely reachable";
    }
    assert(0 && "Invalid reachability");
    return "(unknown)";
}

static
std::string
client_state(Client::RunningState cstate) {
    switch (cstate) {
    case Client::RunningState::Created:
        return "created";
    case Client::RunningState::Failed:
        return "failed";
    case Client::RunningState::Starting:
        return "starting";
    case Client::RunningState::Degraded:
        return "degraded";
    case Client::RunningState::Started:
        return "started";
    case Client::RunningState::Stopping:
        return "stopping";
    case Client::RunningState::Stopped:
        return "stopped";
    }
    assert(0 && "Invalid client state");
    return "(unknown)";
}

void ClientFrontEnd::handle_group_list( const Request&
                                      , Response& res
                                      , std::ostringstream& ss
                                      , cache::Client* cache_client)
{
    res.set(http::field::content_type, "text/plain");

    if (!cache_client) return;

    for (const auto& g : cache_client->get_groups())
        ss << g << std::endl;
}

void ClientFrontEnd::handle_pinned_list( const Request&
                                       , Response& res
                                       , std::ostringstream& ss
                                       , cache::Client* cache_client)
{
    res.set(http::field::content_type, "text/plain");

    if (!cache_client) return;

    for (const auto& g : cache_client->get_pinned_groups())
        ss << g << std::endl;
}

std::map<std::string, std::string, std::less<>> get_query(std::string_view target) {

    auto separator = target.find('?');

    std::map<std::string, std::string, std::less<>> query;
    auto npos = std::string_view::npos;

    if (separator == npos) {
        return query;
    }

    target = target.substr(separator + 1);

    while (!target.empty()) {
        separator = target.find('&');

        std::string_view entry = target.substr(0, separator);

        if (separator != npos) {
            target = target.substr(separator + 1);
        } else {
            target = target.substr(target.size());
        }

        separator = entry.find('=');

        if (separator == npos) {
            continue;
        }

        std::string_view key = entry.substr(0, separator);
        std::string_view val = entry.substr(separator + 1);

        if (key.empty()) continue;

        query[std::string(key)] = std::string(val);
    }

    return query;
}

std::optional<bool> parse_enable(std::string& str) {
    if (str == "enable") {
        return true;
    } else if (str == "disable") {
        return false;
    } else {
        return {};
    }
}

void ClientFrontEnd::handle_portal( ClientConfig& config
                                  , Client::RunningState cstate
                                  , boost::optional<UdpEndpoint> local_ep
                                  , const std::shared_ptr<UPnPs>& upnps_ptr
                                  , const bittorrent::DhtBase* dht
                                  , const util::UdpServerReachabilityAnalysis* reachability
                                  , const Request& req, Response& res, ostringstream& ss
                                  , cache::Client* cache_client
                                  , ClientFrontEndMetricsController& metrics
                                  , Cancel cancel
                                  , YieldContext yield)
{
    res.set(http::field::content_type, "text/html");

    auto target = req.target();

    if (_log_level_input->update(target)) {
        config.log_level(_log_level_input->current_value);
        if (config.is_log_file_enabled())  // remember explicitly set level
            _log_level_no_file = _log_level_input->current_value;
    }

    auto query = get_query(target);

    bool query_handled = false;

    std::map<std::string_view, std::function<void (bool)>> bool_handlers = {
        { "origin_access",     [&](bool enable) { config.is_origin_access_enabled(enable);                           } },
        { "proxy_access",      [&](bool enable) { config.is_proxy_access_enabled(enable);                            } },
        { "injector_access",   [&](bool enable) { config.is_injector_access_enabled(enable);                         } },
        { "distributed_cache", [&](bool enable) { config.is_cache_access_enabled(enable);                            } },
        { "auto_refresh",      [&](bool enable) { _auto_refresh_enabled = enable;                                    } },
        { "logfile",           [&](bool enable) { enable ? enable_log_to_file(config) : disable_log_to_file(config); } },
        { "metrics",           [&](bool enable) { enable ? metrics.enable() : metrics.disable();                     } },
    };

    for (auto [name, handler] : bool_handlers) {
        if (auto it = query.find(name); it != query.end()) {
            auto enable = parse_enable(it->second);
            if (!enable.has_value()) {
                res.result(http::status::bad_request);
                ss << it->first << " accepts {enable,disable}, given \"" << it->second << "\"";
                return;
            }
            handler(*enable);
            query_handled = true;
        }
    }

    if (auto it = query.find("purge_cache"); it != query.end() && cache_client) {
        sys::error_code ec;
        auto yield_ = static_cast<asio::yield_context>(yield);
        cache_client->local_purge(cancel, static_cast<asio::yield_context>(yield_[ec]));
        if (!ec && cancel) ec = asio::error::operation_aborted;
        if (ec = asio::error::operation_aborted) return or_throw(yield_, ec);
        query_handled = true;
    }

    if (auto it = query.find("bt_extra_bootstrap"); it != query.end() && cache_client) {
        auto eqpos = target.rfind('=');
        set_bt_extra_bootstraps(target.substr(eqpos + 1), config);
        query_handled = true;
    }

    if (query_handled) {
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

    ss << ToggleInput{"<u>A</u>uto refresh", 'a',      "auto_refresh", _auto_refresh_enabled};

    ss << "<h2>Request mechanisms</h2>\n";
    ss << ToggleInput{"<u>O</u>rigin access",'o',      "origin_access", config.is_origin_access_enabled()};
    ss << ToggleInput{"<u>P</u>roxy access", 'p',      "proxy_access", config.is_proxy_access_enabled()};
    ss << ToggleInput{"<u>I</u>njector proxy", 'i',    "injector_access", config.is_injector_access_enabled()};
    ss << ToggleInput{"Distributed <u>C</u>ache", 'c', "distributed_cache", config.is_cache_access_enabled()};

    ss << "<h2>Logging</h2>\n";
    ss << *_log_level_input;
    bool log_file_enabled = config.is_log_file_enabled();
    ss << ToggleInput{"<u>L</u>og file", 'l', "logfile", log_file_enabled};
    if (log_file_enabled)
        ss << "Logging debug output to file: " << as_safe_html(logger.current_log_file())
           << " <a href=\"" << log_file_apath << "\" class=\"download\" download=\"ouinet-logfile.txt\">"
           << "Download log file" << "</a><br>\n";

    ss << "<h2>Ouinet client</h2>\n";
    ss << "State: " << client_state(cstate)  << "<br>\n";
    ss << "Version: " << Version::VERSION_NAME << " " << Version::BUILD_ID << "<br>\n";
    ss << "Protocol: " << http_::protocol_version_current << "<br>\n";
    ss << "Now: " << now_as_string()  << "<br>\n";

    ss << "<h2>Network</h2>\n";

    if (reachability) {
        ss << "Reachability status: " << reachability_status(*reachability) << "<br>\n";
    }
    if (local_ep) {
        ss << "Local UDP endpoints:<br>\n";
        ss << "<ul>\n";
        for (auto& ep : local_udp_endpoints(*local_ep))
            ss << "<li>" << as_safe_html(ep) << "</li>\n";
        ss << "</ul>\n";
    }

    if (upnps_ptr)
    {
        const auto& upnps = *upnps_ptr;
        auto upnp_status_ = upnp_status(upnps);
        ss << "UPnP status: " << upnp_status_ << "<br>\n";

        if (upnp_status_ == "enabled")
        {
            ss << "External UDP endpoints (from UPnP):<br>\n";
        ss << "<ul>\n";
        for (auto& ep : external_udp_endpoints(upnps))
            ss << "<li>" << as_safe_html(ep) << "</li>\n";
        ss << "</ul>\n";
        }
    }

    if (dht) {
        ss << "Public UDP endpoints (from BitTorrent DHT):<br>\n";
        ss << "<ul>\n";
        for (auto& ep : public_udp_endpoints(*dht))
            ss << "<li>" << as_safe_html(ep) << "</li>\n";
        ss << "</ul>\n";
    }

    ss << "BEP5 announcements of this client as a bridge are ";
    if (config.is_bridge_announcement_enabled())
        ss << "enabled.<br>\n";
    else
        ss << "disabled.<br>\n";
    ss << "<br>\n";

    ss << "Injector endpoint: " << config.injector_endpoint() << "<br>\n";
    if (auto doh_ep = config.origin_doh_endpoint()) {
        ss << "Origin <abbr title=\"DNS over HTTPS\">DoH</abbr> endpoint URL:"
           << " <samp>" << as_safe_html(*doh_ep) << "</samp><br>\n";
    }

    ss << TextInput{ "BitTorrent extra <u>b</u>ootstraps (space-separated, applied on restart)", 'b'
                   , "bt_extra_bootstraps"
                   , "HOST1 HOST2:PORT ..."
                   , get_bt_extra_bootstraps(config)};

    if (_show_pending_tasks) {
        ss << "        <h2>Pending tasks " << _pending_tasks.size() << "</h2>\n";
        ss << "        <ul>\n";
        for (auto& task : _pending_tasks) {
            ss << "            <li><pre>" << task << "</pre></li>\n";
        }
        ss << "        </ul>\n";
    }

    if (cache_client) {
        ss << "<h2>Distributed cache</h2>\n";
        auto inj_pubkey = config.cache_http_pub_key();
        if (inj_pubkey) {
            auto inj_pubkey_s = inj_pubkey->serialize();
            ss << "Injector pubkey (hex): " << util::bytes::to_hex(inj_pubkey_s) << "<br>\n";
            ss << "Injector pubkey (Base32): " << util::base32up_encode(inj_pubkey_s) << "<br>\n";
            ss << "<br>\n";
        }

        ss << ( boost::format("BEP5 Announcements are sent to the DHT in batches of %s. <br>\n")
                % config.max_simultaneous_announcements());
        ss << "<br>\n";

        auto max_age = config.max_cached_age();
        ss << ( boost::format("Content cached locally if newer than %d seconds"
                              " (i.e. not older than %s).<br>\n")
              % max_age.total_seconds() % past_as_string(max_age));

        sys::error_code ec;
        auto yield_ = static_cast<asio::yield_context>(yield);
        auto local_size = cache_client->local_size(cancel, yield_[ec]);
        if (!ec && cancel) ec = asio::error::operation_aborted;
        if (ec == asio::error::operation_aborted) return or_throw(yield_, ec);

        ss << "Approximate size of content cached locally: ";
        if (ec) ss << "(unknown)";
        else ss << (boost::format("%.02f MiB") % (local_size / 1048576.));
        ss << "<br>\n";

        ss << "<form method=\"get\">\n"
              "    <input type=\"submit\" "
                         "name=\"purge_cache\" id=\"input-purge_cache\" "
                         "value=\"Purge cache now\"/>\n"
              "</form>\n";
        ss << "<br>\n";

        ss << "See DHT groups: \n";
        ss << "<ul>\n";
        ss << "<li><a href=\"" << group_list_apath << "\">Announced</a><br></li>\n";
        ss << "<li><a href=\"" << pinned_list_apath << "\">Pinned</a><br></li>\n";
        ss << "</ul>\n";
        ss << "<br>\n";

        if (config.cache_static_path().empty()) {
            ss << "Static cache is not enabled.<br>\n";
        } else {
            ss << "Static cache is enabled:\n";
            ss << "<ul>\n";
            ss << "<li>Root (content): <code>" << as_safe_html(util::str(config.cache_static_content_path())) << "</code></li>\n";
            ss << "<li>Repository: <code>" << as_safe_html(util::str(config.cache_static_path())) << "</code></li>\n";
            ss << "</ul>\n";
        }
    }

    // Metrics
    ss << "<h2>Metrics</h2>\n";
    ss << ToggleInput{"<u>M</u>etrics",'m', "metrics", metrics.is_enabled()};

    // Highlight the label/form containing the input selected via the URL fragment.
    ss << "<script>var eid = window.location.hash.substr(1); "
          "if (eid) { var e = document.getElementById(eid); "
                     "if (e) e.parentElement.style.backgroundColor = \"yellow\"; }</script>\n";

    ss << "    </body>\n"
          "</html>\n";
}

void ClientFrontEnd::handle_api_status( ClientConfig& config
                                      , Client::RunningState cstate
                                      , boost::optional<UdpEndpoint> local_ep
                                      , const std::shared_ptr<UPnPs>& upnps_ptr
                                      , const bittorrent::DhtBase* dht
                                      , const util::UdpServerReachabilityAnalysis* reachability
                                      , const Request& req, Response& res, ostringstream& ss
                                      , cache::Client* cache_client
                                      , std::shared_ptr<ouiservice::Bep5Client> client
                                      , ClientFrontEndMetricsController& metrics
                                      , Cancel cancel
                                      , YieldContext yield)
{
    res.set(http::field::content_type, "application/json");

    json response = {
        {"auto_refresh", _auto_refresh_enabled},
        {"origin_access", config.is_origin_access_enabled()},
        {"proxy_access", config.is_proxy_access_enabled()},
        {"injector_access", config.is_injector_access_enabled()},
        {"injector_peers_n", client -> injector_candidates_n()},
        {"injector_ready", client -> injector_candidates_n() > 1},
        {"distributed_cache", config.is_cache_access_enabled()},
        {"max_cached_age", config.max_cached_age().total_seconds()},
        {"ouinet_version", Version::VERSION_NAME},
        {"ouinet_build_id", Version::BUILD_ID},
        {"ouinet_protocol", http_::protocol_version_current},
        {"state", client_state(cstate)},
        {"logfile", config.is_log_file_enabled()},
        {"bridge_announcement", config.is_bridge_announcement_enabled()},
        {"metrics_enabled", metrics.is_enabled()}
    };

    if (local_ep) response["local_udp_endpoints"] = local_udp_endpoints(*local_ep);

    if (upnps_ptr)
    {
        const auto& upnps = *upnps_ptr;
        auto upnp_status_ = upnp_status(upnps);
        response["is_upnp_active"] = upnp_status_;
        if (upnp_status_ == "enabled")
            response["external_udp_endpoints"] = external_udp_endpoints(upnps);
    }

    if (dht) response["public_udp_endpoints"] = public_udp_endpoints(*dht);

    response["bt_extra_bootstraps"] = bt_extra_bootstraps(config);

    if (reachability) response["udp_world_reachable"] = reachability_status(*reachability);

    if (auto record_id = metrics.current_record_id(); record_id.has_value()) {
        response["current_metrics_record_id"] = *record_id;
    }

    if (cache_client) {
        sys::error_code ec;
        auto yield_ = static_cast<asio::yield_context>(yield);
        auto sz = cache_client->local_size(cancel, yield_[ec]);
        if (!ec && cancel) ec = asio::error::operation_aborted;
        if (ec == asio::error::operation_aborted) return or_throw(yield_, ec);
        if (ec) {
            LOG_ERROR("Front-end: Failed to get local cache size; ec=", ec);
        } else {
            response["local_cache_size"] = sz;
        }
    }

    ss << response;
}

void ClientFrontEnd::handle_api_groups(std::string_view sub_path
                                      , const Request& req
                                      , Response& res
                                      , ostringstream& ss
                                      , cache::Client* cache_client)
{
    res.set(http::field::content_type, "application/json");

    sys::error_code ec;
    json response{};
    auto error_response = [&res, &response, &ss]( const std::string& error_message
                                                , const http::status& status)
    {
        res.result(status);
        response = {{"status", "error"}, {"message", error_message}};
        ss << response;
    };

    auto query = get_query(req.target());

    string group_name;
    if (const auto name = query.find("name"); name != query.end())
    {
        group_name = name->second;
        response["name"] = group_name;
    }

    if (!cache_client)
    {
        error_response( "Cache client error"
                      , http::status::internal_server_error);
        return;
    }

    if (sub_path == "/" || sub_path.empty())
    {
        response["groups"] = json::array();
        for (const auto& g : cache_client->get_groups())
            response["groups"].push_back(g);
    }
    else if (sub_path.starts_with("/pinned"))
    {
        if (group_name.empty())
        {
            response["pinned_groups"] = json::array();
            for (const auto& g : cache_client->get_pinned_groups())
                response["pinned_groups"].push_back(g);
        }
        else
        {
            response["pinned"] = cache_client->is_pinned_group(group_name, ec);
        }
    }
    else if (sub_path.starts_with("/pin"))
    {
        response["pinned"] = cache_client->pin_group(group_name, ec);
    }
    else if (sub_path.starts_with("/unpin"))
    {
        bool unpinned = cache_client->unpin_group(group_name, ec);
        response["pinned"] = !unpinned;
    }
    else
    {
        error_response( "Undefined action"
                      , http::status::not_found);
        return;
    }

    if (ec)
    {
        auto status = http::status::internal_server_error;
        if (ec.value() == sys::errc::no_such_file_or_directory)
            status = http::status::not_found;
        error_response(ec.message(), status);
        return;
    }

    ss << response;
}

void ClientFrontEnd::handle_api_metrics( std::string_view sub_path
                                       , const Request& req, Response& res, ostringstream& ss
                                       , ClientFrontEndMetricsController& metrics
                                       , Cancel cancel
                                       , YieldContext yield)
{
    res.set(http::field::content_type, "text/html");

    if (sub_path.starts_with("/set_key_value")) {
        auto query = get_query(req.target());

        auto rec_it = query.find("record_id");
        auto key_it = query.find("key");
        auto val_it = query.find("value");

        bool missing = false;

        if (rec_it == query.end()) missing = true;
        if (key_it == query.end()) missing = true;
        if (val_it == query.end()) missing = true;

        if (missing) {
            res.result(http::status::bad_request);
            ss << "set_key_value requires \"record_id\", \"key\" and \"value\" arguments\n";
            return;
        }

        auto result = metrics.set_aux_key_value(rec_it->second, key_it->second, val_it->second);

        switch (result) {
            case metrics::SetAuxResult::Ok:
                return; // all good
            case metrics::SetAuxResult::BadRecordId:
                res.result(http::status::conflict);
                ss << "invalid or old record ID, get a new one from /api/status\n";
                return;
            case metrics::SetAuxResult::Noop:
                res.result(http::status::bad_request);
                ss << "metrics not enabled\n";
                return;
        }
    }

    ss << "invalid API command to metrics (" << sub_path << ")\n";
    res.result(http::status::bad_request);
}

Response ClientFrontEnd::serve( ClientConfig& config
                              , const Request& req
                              , Client::RunningState client_state
                              , cache::Client* cache_client
                              , std::shared_ptr<ouiservice::Bep5Client> client
                              , const CACertificate& ca
                              , boost::optional<UdpEndpoint> local_ep
                              , const std::shared_ptr<UPnPs>& upnps_ptr
                              , const bittorrent::DhtBase* dht
                              , const util::UdpServerReachabilityAnalysis* reachability
                              , ClientFrontEndMetricsController& metrics
                              , Cancel cancel
                              , YieldContext yield)
{
    if (auto& token = config.front_end_access_token()) {
        std::string_view header_key = "X-Ouinet-Front-End-Token";
        if (*token != req[header_key]) {
            Response res{http::status::forbidden, req.version()};
            res.keep_alive(false);

            auto body = std::string("The request is missing a valid ")
                      + std::string(header_key)
                      + " HTTP header\n";

            Response::body_type::reader reader(res, res.body());
            sys::error_code ec;
            reader.put(asio::buffer(body), ec);
            assert(!ec);

            res.prepare_payload();
            return res;
        }
    }

    Response res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.keep_alive(false);

    ostringstream ss;

    auto url = util::Url::from(req.target());

    auto path_str = (url && !url->path.empty()) ? url->path : std::string(req.target());
    std::string_view path(path_str);

    std::string_view groups_api_path = "/api/groups";
    std::string_view metrics_api_path = "/api/metrics";
    std::string_view status_api_path = "/api/status";

    if (path == "/ca.pem") {
        handle_ca_pem(req, res, ss, ca);
    } else if (path == log_file_apath) {
        res.set(http::field::content_type, "text/plain");
        load_log_file(config, ss);
    } else if (path == group_list_apath) {
        handle_group_list(req, res, ss, cache_client);
    } else if (path == pinned_list_apath) {
        handle_pinned_list(req, res, ss, cache_client);
    } else if (path == status_api_path) {
        sys::error_code e;
        handle_api_status( config, client_state, local_ep, upnps_ptr, dht, reachability
                         , req, res, ss, cache_client, client, metrics, cancel
                         , yield[e]);
    } else if (path.starts_with(groups_api_path)) {
        path.remove_prefix(groups_api_path.size());
        handle_api_groups(path, req, res, ss, cache_client);
    } else if (path.starts_with(metrics_api_path)) {
        path.remove_prefix(metrics_api_path.size());
        sys::error_code e;
        handle_api_metrics(path, req, res, ss, metrics, cancel , yield[e]);
    } else {
        sys::error_code e;
        handle_portal( config, client_state, local_ep, upnps_ptr, dht, reachability
                     , req, res, ss, cache_client, metrics, cancel
                     , yield[e]);
    }

    Response::body_type::reader reader(res, res.body());
    sys::error_code ec;
    reader.put(asio::buffer(ss.str()), ec);
    assert(!ec);

    res.prepare_payload();

    return res;
}

ClientFrontEnd::~ClientFrontEnd() {}

