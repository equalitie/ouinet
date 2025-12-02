#pragma once

#include <set>
#include <sstream>
#include <vector>

#include <boost/asio/ssl/context.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include "declspec.h"
#include "namespaces.h"
#include "cache_control.h"
#include "util.h"
#include "util/bytes.h"
#include "parse/endpoint.h"
#include "util/crypto.h"
#ifndef __WIN32
#include "increase_open_file_limit.h"
#endif
#include "endpoint.h"
#include "logger.h"
#include "constants.h"
#include "bep5_swarms.h"
#include "util.h"
#include "bittorrent/bootstrap.h"

namespace ouinet {

#define _LOG_FILE_NAME "log.txt"
static const fs::path log_file_name{_LOG_FILE_NAME};

#define _DEFAULT_STATIC_CACHE_SUBDIR ".ouinet"
static const fs::path default_static_cache_subdir{_DEFAULT_STATIC_CACHE_SUBDIR};

struct MetricsConfig {
    bool enable_on_start = false;
    util::Url server_url;
    boost::optional<std::string> server_token;
    boost::optional<asio::ssl::context> server_cacert;
    metrics::EncryptionKey encryption_key;

    static std::unique_ptr<MetricsConfig> parse(const boost::program_options::variables_map&);
};

struct OuisyncCacheConfig {
    // Read token for the page index repository which contains directories one per host name
    // and inside them crawls of corresponding websites.
    std::string page_index_token;
};

class OUINET_DECL ClientConfig {
public:
    enum class CacheType { None, Bep5Http, Ouisync };

    ClientConfig() = default;

    // Throws on error
    ClientConfig(int argc, const char* argv[]);

    ClientConfig(ClientConfig&&) = default;
    ClientConfig& operator=(ClientConfig&&) = default;

    ClientConfig(const ClientConfig&) = delete;
    ClientConfig& operator=(const ClientConfig&) = delete;

    const fs::path& repo_root() const {
        return _repo_root;
    }

    const boost::optional<Endpoint>& injector_endpoint() const {
        return _injector_ep;
    }

    const std::string& tls_injector_cert_path() const {
        return _tls_injector_cert_path;
    }

    const std::string& tls_ca_cert_store_path() const {
        return _tls_ca_cert_store_path;
    }

    const asio::ip::tcp::endpoint& local_endpoint() const {
        return _local_ep;
    }

    boost::optional<uint16_t> udp_mux_port() const {
        return _udp_mux_port;
    }

    bool is_cache_enabled() const { return _cache_type != CacheType::None; }
    CacheType cache_type() const { return _cache_type; }

    boost::posix_time::time_duration max_cached_age() const {
        return _max_cached_age;
    }

    size_t max_simultaneous_announcements() const {
        return _max_simultaneous_announcements;
    }

    bool do_cache_private() const {
        return _cache_private;
    }

    const fs::path& cache_static_path() const {
        return _cache_static_path;
    }

    const fs::path& cache_static_content_path() const {
        return _cache_static_content_path;
    }

    boost::optional<std::string> bep5_bridge_swarm_name() {
        if (!_cache_http_pubkey) return boost::none;
        return bep5::compute_bridge_swarm_name( *_cache_http_pubkey
                                              , http_::protocol_version_current);
    }

    bool is_bridge_announcement_enabled() const {
        return !_disable_bridge_announcement;
    }

    boost::optional<std::string>
    credentials_for(const Endpoint& injector) const {
        auto i = _injector_credentials.find(injector);
        if (i == _injector_credentials.end()) return {};
        return i->second;
    }

    asio::ip::tcp::endpoint front_end_endpoint() const {
        return _front_end_endpoint;
    }

    const boost::optional<std::string>& front_end_access_token() const {
        return _front_end_access_token;
    }

    const boost::optional<std::string>& proxy_access_token() const {
        return _proxy_access_token;
    }

    boost::optional<util::Ed25519PublicKey> cache_http_pub_key() const {
        return _cache_http_pubkey;
    }

    const std::string& client_credentials() const { return _client_credentials; }

    std::string local_domain() const { return _local_domain; }

    bool is_doh_enabled() const {
        return !_disable_doh;
    }

    uint64_t max_request_body_size() const {
        // The value is set in KiB in the configuration
        // and used in bytes by boost::beast
        return _max_req_body_size * 1024;
    }

    bool is_help() const { return _is_help; }

    auto description() {
        return description_full();
    }

    // Is `nullptr` if metrics is disabled
    const MetricsConfig* metrics() const {
        return _metrics.get();
    }

    // Is `nullptr` if metrics is disabled
    MetricsConfig* metrics() {
        return _metrics.get();
    }

    // Use when debugging to add HTTP header fields to every request
    const std::map<std::string, std::string>& add_request_fields() const {
        return _add_request_fields;
    }

    const std::optional<OuisyncCacheConfig>& ouisync_cache_config() const {
        return _ouisync;
    }

private:
    boost::program_options::options_description description_full()
    {
        using namespace std;
        namespace po = boost::program_options;

        po::options_description general("General options");
        general.add_options()
           ("help", "Produce this help message")
           ("repo", po::value<string>(), "Path to the repository root")
           ("drop-saved-opts", po::bool_switch()->default_value(false)
            , "Drop saved persistent options right before start "
              "(only use command line arguments and configuration file)")
           ("log-level", po::value<string>()->default_value(util::str(default_log_level()))
            , "Set log level: silly, debug, verbose, info, warn, error, abort. "
              "This option is persistent.")
           ("enable-log-file", po::bool_switch()->default_value(false)
            , "Enable writing log messages to "
              "log file \"" _LOG_FILE_NAME "\" under the repository root. "
              "This option is persistent.")
           ("bt-bootstrap-extra", po::value<vector<string>>()->composing()
            , "Extra BitTorrent bootstrap server (in <HOST> or <HOST>:<PORT> format) "
              "to start the DHT (can be used several times). "
              "<HOST> can be a host name, <IPv4> address, or <[IPv6]> address. "
              "This option is persistent.")
#ifndef __WIN32
           ("open-file-limit"
            , po::value<unsigned int>()
            , "To increase the maximum number of open files")
#endif
           ;

        po::options_description services("Service options");
        services.add_options()
           ("listen-on-tcp"
            , po::value<string>()->default_value("127.0.0.1:8077")
            , "HTTP proxy endpoint (in <IP>:<PORT> format)")
           ("udp-mux-port"
           , po::value<uint16_t>()
           , "Port used by the UDP multiplexer in BEP5 and uTP interactions.")
           ("client-credentials", po::value<string>()
            , "<username>:<password> authentication pair for the client")
           ("tls-ca-cert-store-path", po::value<string>(&_tls_ca_cert_store_path)
            , "Path to the CA certificate store file")
           ("front-end-ep"
            , po::value<string>()->default_value("127.0.0.1:8078")
            , "Front-end's endpoint (in <IP>:<PORT> format)")
           ("front-end-access-token"
            , po::value<string>()
            , "Token to access the front end, use agents will need to include the X-Ouinet-Front-End-Token "
              "with the value of this string in http request headers or get the \"403 Forbidden\" response.")
           ("proxy-access-token"
            , po::value<string>()
            , "Token to access the http proxy, use agents will need to include the X-Ouinet-Proxy-Token "
              "with the value of this string in http request headers or get the \"403 Forbidden\" response.")
           ("disable-bridge-announcement"
            , po::bool_switch(&_disable_bridge_announcement)->default_value(false)
            , "Disable BEP5 announcements of this client to the Bridges list in the DHT. "
              "Previous announcements could take up to an hour to expire.")
           ("request-body-limit"
            , po::value<uint64_t>()->default_value(_max_req_body_size)
            , "Set the max size of body requests in KiB. This could be "
              "useful to handle big POST/PUT requests from the UA, e.g. non-chunked "
              "uploads, etc. To leave it unlimited, set it to zero.")
           ("add-request-field"
            , po::value<std::vector<std::string>>()->composing()
            , "A <FIELD>:<VALUE> pair representing a HTTP header field and "
              "value to add to every request coming from the User Agent before "
              "Ouinet processes it. Useful for testing when using e.g. Firefox "
              "as the UA.")
           ;

        po::options_description injector("Injector options");
        injector.add_options()
           ("injector-ep"
            , po::value<string>()
            , "Injector's endpoint as <TYPE>:<EP>, "
              "where <TYPE> can be \"tcp\", \"utp\",  "
#ifdef __EXPERIMENTAL__
              "\"obfs2\", \"obfs3\", \"obfs4\", \"lampshade\" or \"i2p\", "
#endif // ifdef __EXPERIMENTAL__
              "and <EP> depends on the type of endpoint: "
              "<IP>:<PORT> for TCP and uTP"
#ifdef __EXPERIMENTAL__
              ", <IP>:<PORT>[,<OPTION>=<VALUE>...] for OBFS and Lampshade, "
              "<B32_PUBKEY>.b32.i2p or <B64_PUBKEY> for I2P"
#endif // ifdef __EXPERIMENTAL__
           )
           ("injector-credentials", po::value<string>()
            , "<username>:<password> authentication pair for the injector")
           ("injector-tls-cert-file", po::value<string>(&_tls_injector_cert_path)
            , "Path to the injector's TLS certificate; enable TLS for TCP and uTP")
           ;

        po::options_description cache("Cache options");
        cache.add_options()
           ("cache-type", po::value<string>()->default_value("none")
            , "Type of d-cache {none, bep5-http, ouisync}")
           ("cache-http-public-key"
            , po::value<string>()
            , "Public key for HTTP signatures in the BEP5/HTTP cache "
              "(hex-encoded or Base32-encoded)")
           ("max-cached-age"
            , po::value<int>()->default_value(_max_cached_age.total_seconds())
            , "Discard cached content older than this many seconds "
              "(0: discard all; -1: discard none)")
           ("max-simultaneous-announcements"
            , po::value<int>()->default_value(_max_simultaneous_announcements)
            , "Defines the number of simultaneous BEP5 announcements "
              "performed by the announcer loop to the DHT.")
          ("cache-private"
           , po::bool_switch(&_cache_private)->default_value(false)
           , "Store responses regardless of being private or "
             "the result of an authorized request "
             "(in spite of Section 3 of RFC 7234), "
             "unless tagged as private to the Ouinet client. "
             "Sensitive headers are still removed from Injector requests. "
             "May need special injector configuration. "
             "USE WITH CAUTION.")
          ("cache-static-repo"
           , po::value<string>()
           , "Repository for internal files of the static cache "
             "(to use as read-only fallback for the local cache); "
             "if this is not given but a static cache content root is, "
             "\"" _DEFAULT_STATIC_CACHE_SUBDIR "\" under that directory is assumed.")
          ("cache-static-root"
           , po::value<string>()
           , "Root directory for content files of the static cache. "
             "The static cache always requires this (even if empty).")
          ("ouisync-page-index", po::value<string>(),
           "A Ouisync repository read token. The repository contains files with names "
           "corresponding to domains and each one contains another read token to a "
           "repository with a scrape of that domain")
          ;

        po::options_description requests("Request options");
        requests.add_options()
           ("disable-origin-access", po::bool_switch(&_disable_origin_access)->default_value(false)
            , "Disable direct access to the origin (forces use of injector and the cache). "
              "This option is persistent.")
           ("disable-injector-access", po::bool_switch(&_disable_injector_access)->default_value(false)
            , "Disable access to the injector. "
              "This option is persistent.")
           ("disable-cache-access", po::bool_switch(&_disable_cache_access)->default_value(false)
            , "Disable access to cached content. "
              "This option is persistent.")
           ("disable-proxy-access", po::bool_switch(&_disable_proxy_access)->default_value(false)
            , "Disable proxied access to the origin (via the injector). "
              "This option is persistent.")
           ("local-domain"
            , po::value<string>()->default_value("local")
            , "Always use origin access and never use cache for this TLD")
           ("disable-doh", po::bool_switch(&_disable_doh)->default_value(false)
            , "Disable DNS over HTTPS for origin access and bootstrap domain resolution. "
              "When this option is present the client will fallback to the default DNS mechanism "
              "provided by the operating system.")
            ("allow-private-targets", po::bool_switch(&_allow_private_targets)->default_value(false)
            , "Allows using non-origin channels, like injectors, dist-cache, etc, "
              "to fetch targets using private addresses. "
              "Example: 192.168.1.13, 10.8.0.2, 172.16.10.8, etc.")
           ;

        po::options_description metrics("Metrics options");
        metrics.add_options()
           ("metrics-enable-on-start", po::bool_switch()->default_value(false)
            , "Enable metrics at startup. Must be used with --metrics-server-url")
           ("metrics-server-url", po::value<string>()
            , "URL to the metrics server where statistics/metrics records will be sent over HTTP.")
           ("metrics-server-token", po::value<string>()
            , "Token sent to the server as 'token: <TOKEN>' HTTP header.")
           ("metrics-server-cacert", po::value<string>()
            , "Tls CA certificate for the metrics server")
           ("metrics-server-cacert-file", po::value<string>()
            , "File containing the CA certificate for the metrics server")
           ("metrics-encryption-key", po::value<string>()
            , "Key to encrypt metrics records with. To generate the (public) encryption key, you can use "
              "the following. \n"
              "   First generate the private key:\n"
              "     `openssl genpkey -algorithm x25519 -out private_key.pem`\n"
              "   Then get the public encryption key:\n"
              "     `openssl pkey -in private_key.pem -pubout -out public_key.pem`"
              )
           ;

        po::options_description desc;
        desc
            .add(general)
            .add(services)
            .add(injector)
            .add(cache)
            .add(requests)
            .add(metrics);
        return desc;
    }

    // A restricted version of the above, only accepting persistent configuration options,
    // with no defaults nor descriptions.
    boost::program_options::options_description description_saved()
    {
        namespace po = boost::program_options;

        po::options_description desc;
        desc.add_options()
            ("log-level", po::value<std::string>())
            ("enable-log-file", po::bool_switch())
            ("bt-bootstrap-extra", po::value<std::vector<std::string>>()->composing())
            ("disable-origin-access", po::bool_switch(&_disable_origin_access))
            ("disable-injector-access", po::bool_switch(&_disable_injector_access))
            ("disable-cache-access", po::bool_switch(&_disable_cache_access))
            ("disable-proxy-access", po::bool_switch(&_disable_proxy_access))
            ;
        return desc;
    }

    void save_persistent() {
        using namespace std;
        ostringstream ss;

        ss << "log-level = " << log_level() << endl;
        ss << "enable-log-file = " << is_log_file_enabled() << endl;

        for (const auto& btbs_addr : _bt_bootstrap_extras)
            ss << "bt-bootstrap-extra = " << btbs_addr << endl;

        ss << "disable-origin-access = " << _disable_origin_access << endl;
        ss << "disable-injector-access = " << _disable_injector_access << endl;
        ss << "disable-cache-access = " << _disable_cache_access << endl;
        ss << "disable-proxy-access = " << _disable_proxy_access << endl;

        try {
            fs::path ouinet_save_path = _repo_root/_ouinet_conf_save_file;
            LOG_DEBUG("Saving persistent options");
            ofstream(ouinet_save_path.string(), fstream::out | fstream::trunc) << ss.str();
        } catch (const exception& e) {
            LOG_ERROR("Failed to save persistent options: ", e.what());
        }
    }

public:
    using ExtraBtBsServers = std::set<bittorrent::bootstrap::Address>;

#define CHANGE_AND_SAVE_OPS(_CMP, _SET) { \
    bool changed = !(_CMP); \
    if (changed) { \
        _SET; \
        save_persistent(); \
    } \
}
#define CHANGE_AND_SAVE(_F, _V) CHANGE_AND_SAVE_OPS((_V) == _F, _F = (_V))

    log_level_t log_level() const { return logger.get_threshold(); }
    void log_level(log_level_t level) { CHANGE_AND_SAVE_OPS(level == logger.get_threshold(), logger.set_threshold(level)); }

    const ExtraBtBsServers& bt_bootstrap_extras() const {
        return _bt_bootstrap_extras;
    }
    void bt_bootstrap_extras(ExtraBtBsServers bts) {
        CHANGE_AND_SAVE_OPS(bts == _bt_bootstrap_extras, _bt_bootstrap_extras = std::move(bts));
    }

    bool is_log_file_enabled() const { return _is_log_file_enabled(); }
    void is_log_file_enabled(bool v) { CHANGE_AND_SAVE_OPS(v == _is_log_file_enabled(), _is_log_file_enabled(v)); }

    bool is_cache_access_enabled() const { return is_cache_enabled() && !_disable_cache_access; }
    void is_cache_access_enabled(bool v) { CHANGE_AND_SAVE(_disable_cache_access, !v); }

    bool is_origin_access_enabled() const { return !_disable_origin_access; }
    void is_origin_access_enabled(bool v) { CHANGE_AND_SAVE(_disable_origin_access, !v); }

    bool is_proxy_access_enabled() const { return !_disable_proxy_access; }
    void is_proxy_access_enabled(bool v) { CHANGE_AND_SAVE(_disable_proxy_access, !v); }

    bool is_injector_access_enabled() const { return !_disable_injector_access; }
    void is_injector_access_enabled(bool v) { CHANGE_AND_SAVE(_disable_injector_access, !v); }

    bool is_private_target_allowed() const { return _allow_private_targets; }

#undef CHANGE_AND_SAVE_OPS
#undef CHANGE_AND_SAVE

private:
    inline bool _is_log_file_enabled() const {
        return logger.get_log_file() != nullptr;
    }

    inline void _is_log_file_enabled(bool v) {
        if (!v) {
            logger.log_to_file("");
            return;
        }

        if (_is_log_file_enabled()) return;

        auto current_log_path = logger.current_log_file();
        auto ouinet_log_path = current_log_path.empty()
            ? (_repo_root / log_file_name).string()
            : current_log_path;

        logger.log_to_file(ouinet_log_path);
        LOG_INFO("Log file set to: ", ouinet_log_path);
    }

private:
    bool _is_help = false;
    fs::path _repo_root;
    fs::path _ouinet_conf_file = "ouinet-client.conf";
    fs::path _ouinet_conf_save_file = "ouinet-client.saved.conf";
    asio::ip::tcp::endpoint _local_ep;
    boost::optional<uint16_t> _udp_mux_port;
    boost::optional<Endpoint> _injector_ep;
    std::string _tls_injector_cert_path;
    std::string _tls_ca_cert_store_path;
    ExtraBtBsServers _bt_bootstrap_extras;
    bool _disable_cache_access = false;
    bool _disable_origin_access = false;
    bool _disable_proxy_access = false;
    bool _disable_injector_access = false;
    asio::ip::tcp::endpoint _front_end_endpoint;
    boost::optional<std::string> _front_end_access_token;
    boost::optional<std::string> _proxy_access_token;
    bool _disable_bridge_announcement = false;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
    size_t _max_simultaneous_announcements
        = default_max_simultaneous_announcements;
    uint64_t _max_req_body_size = 102400;
    bool _cache_private = false;

    std::string _client_credentials;
    std::map<Endpoint, std::string> _injector_credentials;

    fs::path _cache_static_path;
    fs::path _cache_static_content_path;
    boost::optional<util::Ed25519PublicKey> _cache_http_pubkey;
    CacheType _cache_type = CacheType::None;
    std::string _local_domain;
    bool _disable_doh = false;
    bool _allow_private_targets = false;
    std::map<std::string, std::string> _add_request_fields;

    std::unique_ptr<MetricsConfig> _metrics;
    std::optional<OuisyncCacheConfig> _ouisync;
};

#undef _LOG_FILE_NAME
#undef _DEFAULT_STATIC_CACHE_SUBDIR
} // ouinet namespace
