#pragma once

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include "namespaces.h"
#include "cache_control.h"
#include "doh.h"
#include "util.h"
#include "util/bytes.h"
#include "parse/endpoint.h"
#include "util/crypto.h"
#include "increase_open_file_limit.h"
#include "endpoint.h"
#include "logger.h"
#include "constants.h"
#include "bep5_swarms.h"

namespace ouinet {

#define _DEFAULT_STATIC_CACHE_SUBDIR ".ouinet"
static const fs::path default_static_cache_subdir{_DEFAULT_STATIC_CACHE_SUBDIR};

class ClientConfig {
public:
    enum class CacheType { None, Bep5Http };

    ClientConfig();

    // Throws on error
    ClientConfig(int argc, char* argv[]);

    ClientConfig(const ClientConfig&) = default;
    ClientConfig& operator=(const ClientConfig&) = default;

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

    boost::posix_time::time_duration max_cached_age() const {
        return _max_cached_age;
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

    boost::optional<std::string>
    credentials_for(const Endpoint& injector) const {
        auto i = _injector_credentials.find(injector);
        if (i == _injector_credentials.end()) return {};
        return i->second;
    }

    asio::ip::tcp::endpoint front_end_endpoint() const {
        return _front_end_endpoint;
    }

    boost::optional<util::Ed25519PublicKey> cache_http_pub_key() const {
        return _cache_http_pubkey;
    }

    const std::string& client_credentials() const { return _client_credentials; }

    bool is_help() const { return _is_help; }

    boost::program_options::options_description description()
    {
        using namespace std;
        namespace po = boost::program_options;

        po::options_description desc;

        desc.add_options()
           ("help", "Produce this help message")
           ("repo", po::value<string>(), "Path to the repository root")
           ("log-level", po::value<string>()->default_value("INFO"), "Set log level: silly, debug, verbose, info, warn, error, abort")

           // Client options
           ("listen-on-tcp"
            , po::value<string>()->default_value("127.0.0.1:8077")
            , "HTTP proxy endpoint (in <IP>:<PORT> format)")
           ("front-end-ep"
            , po::value<string>()->default_value("127.0.0.1:8078")
            , "Front-end's endpoint (in <IP>:<PORT> format)")
           ("tls-ca-cert-store-path", po::value<string>(&_tls_ca_cert_store_path)
            , "Path to the CA certificate store file")
           ("open-file-limit"
            , po::value<unsigned int>()
            , "To increase the maximum number of open files")

           // Transport options
           ("injector-ep"
            , po::value<string>()
            , "Injector's endpoint as <TYPE>:<EP>, "
              "where <TYPE> can be \"tcp\", \"utp\", \"obfs2\", \"obfs3\", \"obfs4\", \"lampshade\" or \"i2p\", "
              "and <EP> depends on the type of endpoint: "
              "<IP>:<PORT> for TCP and uTP, <IP>:<PORT>[,<OPTION>=<VALUE>...] for OBFS and Lampshade, "
              "<B32_PUBKEY>.b32.i2p or <B64_PUBKEY> for I2P")
           ("client-credentials", po::value<string>()
            , "<username>:<password> authentication pair for the client")
           ("injector-credentials", po::value<string>()
            , "<username>:<password> authentication pair for the injector")
           ("injector-tls-cert-file", po::value<string>(&_tls_injector_cert_path)
            , "Path to the injector's TLS certificate; enable TLS for TCP and uTP")

           // Cache options
           ("cache-type", po::value<string>()->default_value("none")
            , "Type of d-cache {none, bep5-http}")
           ("cache-http-public-key"
            , po::value<string>()
            , "Public key for HTTP signatures in the BEP5/HTTP cache "
              "(hex-encoded or Base32-encoded)")
           ("max-cached-age"
            , po::value<int>()->default_value(_max_cached_age.total_seconds())
            , "Discard cached content older than this many seconds "
              "(0: discard all; -1: discard none)")
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

           // Request routing options
           ("disable-origin-access", po::bool_switch(&_disable_origin_access)->default_value(false)
            , "Disable direct access to the origin (forces use of injector and the cache)")
           ("disable-injector-access", po::bool_switch(&_disable_injector_access)->default_value(false)
            , "Disable access to the injector")
           ("disable-cache-access", po::bool_switch(&_disable_cache_access)->default_value(false)
            , "Disable access to cached content")
           ("disable-proxy-access", po::bool_switch(&_disable_proxy_access)->default_value(false)
            , "Disable proxied access to the origin (via the injector)")
           ("local-domain"
            , po::value<string>()->default_value("local")
            , "Always use origin access and never use cache for this TLD")
           ("origin-doh-base", po::value<string>()
            , "If given, enable DNS over HTTPS for origin access using the given base URL; "
              "the \"dns=...\" query argument will be added for the GET request.")
           ;

        return desc;
    }

    bool cache_enabled() const { return _cache_type != CacheType::None; }
    CacheType cache_type() const { return _cache_type; }

    bool is_cache_access_enabled() const { return cache_enabled() && !_disable_cache_access; }
    void is_cache_access_enabled(bool v) { _disable_cache_access = !v; }

    bool is_origin_access_enabled() const { return !_disable_origin_access; }
    void is_origin_access_enabled(bool v) { _disable_origin_access = !v; }

    bool is_proxy_access_enabled() const { return !_disable_proxy_access; }
    void is_proxy_access_enabled(bool v) { _disable_proxy_access = !v; }

    bool is_injector_access_enabled() const { return !_disable_injector_access; }
    void is_injector_access_enabled(bool v) { _disable_injector_access = !v; }

    std::string local_domain() const { return _local_domain; }

    boost::optional<std::string> origin_doh_endpoint() const {
        return _origin_doh_endpoint;
    }

    bool log_level(const std::string& level) {
        if (level == "SILLY") {
            logger.set_threshold(SILLY);
        } else if (level == "DEBUG") {
            logger.set_threshold(DEBUG);
        } else if (level == "VERBOSE") {
            logger.set_threshold(VERBOSE);
        } else if (level == "INFO") {
            logger.set_threshold(INFO);
        } else if (level == "WARN") {
            logger.set_threshold(WARN);
        } else if (level == "ERROR") {
            logger.set_threshold(ERROR);
        } else if (level == "ABORT") {
            logger.set_threshold(ABORT);
        } else {
            return false;
        }
        return true;
    }

private:
    bool _is_help = false;
    fs::path _repo_root;
    fs::path _ouinet_conf_file = "ouinet-client.conf";
    asio::ip::tcp::endpoint _local_ep;
    boost::optional<Endpoint> _injector_ep;
    std::string _tls_injector_cert_path;
    std::string _tls_ca_cert_store_path;
    bool _disable_cache_access = false;
    bool _disable_origin_access = false;
    bool _disable_proxy_access = false;
    bool _disable_injector_access = false;
    asio::ip::tcp::endpoint _front_end_endpoint;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
    bool _cache_private = false;

    std::string _client_credentials;
    std::map<Endpoint, std::string> _injector_credentials;

    fs::path _cache_static_path;
    fs::path _cache_static_content_path;
    boost::optional<util::Ed25519PublicKey> _cache_http_pubkey;
    CacheType _cache_type = CacheType::None;
    std::string _local_domain;
    boost::optional<doh::Endpoint> _origin_doh_endpoint;
};

inline
ClientConfig::ClientConfig() { }

inline
ClientConfig::ClientConfig(int argc, char* argv[])
{
    using namespace std;
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

    auto desc = description();

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        _is_help = true;
        return;
    }

    if (!vm.count("repo")) {
        throw std::runtime_error(
                util::str("The '--repo' option is missing"));
    }

    _repo_root = fs::path(vm["repo"].as<string>());

    if (!fs::exists(_repo_root)) {
        throw std::runtime_error(
                util::str("No such directory: ", _repo_root));
    }

    if (!fs::is_directory(_repo_root)) {
        throw std::runtime_error(
                util::str("The path is not a directory: ", _repo_root));
    }

    fs::path ouinet_conf_path = _repo_root/_ouinet_conf_file;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        throw std::runtime_error(
                util::str("The path ", _repo_root, " does not contain the "
                         , _ouinet_conf_file, " configuration file"));
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("log-level")) {
        auto level = boost::algorithm::to_upper_copy(vm["log-level"].as<string>());
        if (!log_level(level))
            throw std::runtime_error(util::str("Invalid log level: ", level));
        LOG_INFO("Log level set to: ", level);
    }

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (vm.count("max-cached-age")) {
        _max_cached_age = boost::posix_time::seconds(vm["max-cached-age"].as<int>());
    }

    if (!vm.count("listen-on-tcp")) {
        throw std::runtime_error(
                util::str("The '--listen-on-tcp' option is missing"));
    }

    auto opt_local_ep = parse::endpoint<asio::ip::tcp>(vm["listen-on-tcp"].as<string>());

    if (!opt_local_ep) {
        throw std::runtime_error("Failed to parse local endpoint");
    }

    _local_ep = *opt_local_ep;

    if (vm.count("injector-ep")) {
        auto injector_ep_str = vm["injector-ep"].as<string>();

        if (!injector_ep_str.empty()) {
            auto opt = parse_endpoint(injector_ep_str);

            if (!opt) {
                throw std::runtime_error(util::str(
                        "Failed to parse endpoint: ", injector_ep_str));
            }

            _injector_ep = *opt;
        }
    }

    if (vm.count("front-end-ep")) {
        auto ep_str = vm["front-end-ep"].as<string>();

        if (ep_str.empty()) {
            throw std::runtime_error("The '--front-end-ep' argument must not be empty");
        }

        sys::error_code ec;
        _front_end_endpoint = parse::endpoint<asio::ip::tcp>(ep_str, ec);

        if (ec) {
            throw std::runtime_error(util::str(
                    "Failed to parse endpoint: ", ep_str));
        }
    }

    if (vm.count("client-credentials")) {
        auto cred = vm["client-credentials"].as<string>();

        if (!cred.empty() && cred.find(':') == string::npos) {
            throw std::runtime_error(util::str(
                "The '--client-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred));
        }

        _client_credentials = move(cred);
    }

    auto maybe_set_pk = [&] (const string& opt, auto& pk) {
        if (vm.count(opt)) {
            string value = vm[opt].as<string>();

            using PubKey = util::Ed25519PublicKey;
            pk = PubKey::from_hex(value);

            if (!pk) {  // attempt decoding from Base32
                auto pk_s = util::base32_decode(value);
                if (pk_s.size() == PubKey::key_size) {
                    auto pk_a = util::bytes::to_array<uint8_t, PubKey::key_size>(pk_s);
                    pk = PubKey(std::move(pk_a));
                }
            }

            if (!pk) {
                throw std::runtime_error(
                        util::str("Failed to parse Ed25519 public key: ", value));
            }
        }
    };

    maybe_set_pk("cache-http-public-key", _cache_http_pubkey);

    if (vm.count("cache-type")) {
        auto type_str = vm["cache-type"].as<string>();

        if (type_str == "bep5-http") {
            // https://redmine.equalit.ie/issues/14920#note-1
            _cache_type = CacheType::Bep5Http;

            LOG_DEBUG("Using bep5-http cache");

            if (!_cache_http_pubkey) {
                throw std::runtime_error(
                    "'--cache-type=bep5-http' must be used with '--cache-http-public-key'");
            }

            if (_injector_ep && _injector_ep->type == Endpoint::Bep5Endpoint) {
                throw std::runtime_error(
                    util::str("A BEP5 injector endpoint is derived implicitly"
                        " when using '--cache-type=bep5-http',"
                        " but it is already set to: ", *_injector_ep));
            }
            if (!_injector_ep) {
                _injector_ep = Endpoint{
                    Endpoint::Bep5Endpoint,
                    bep5::compute_injector_swarm_name(*_cache_http_pubkey, http_::protocol_version_current)
                };
            }
        }
        else if (type_str == "none" || type_str == "") {
            _cache_type = CacheType::None;
        }
        else {
            throw std::runtime_error(
                    util::str("Unknown '--cache-type' argument: ", type_str));
        }

    }

    if (vm.count("injector-credentials")) {
        auto cred = vm["injector-credentials"].as<string>();

        if (!cred.empty()
          && cred.find(':') == string::npos) {
            throw std::runtime_error(util::str(
                "The '--injector-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred));
        }

        if (!_injector_ep) {
            throw std::runtime_error(
                "The '--injector-credentials' argument must be used with "
                "'--injector-ep'");
        }

        _injector_credentials[*_injector_ep] = cred;
    }

    if (_cache_type == CacheType::None) {
        LOG_WARN("Not using d-cache");
    }

    if (cache_enabled() && _cache_type == CacheType::Bep5Http && !_cache_http_pubkey) {
        throw std::runtime_error("BEP5/HTTP cache selected but no injector HTTP public key specified");
    }

    if (vm.count("cache-static-root")) {
        _cache_static_content_path = vm["cache-static-root"].as<string>();
        if (!fs::is_directory(_cache_static_content_path))
            throw std::runtime_error(
                util::str("No such directory: ", _cache_static_content_path));
        if (!vm.count("cache-static-repo")) {
            _cache_static_path = _cache_static_content_path / default_static_cache_subdir;
            LOG_INFO("No static cache repository given, assuming: ", _cache_static_path);
        }
    }
    if (vm.count("cache-static-repo")) {
        _cache_static_path = vm["cache-static-repo"].as<string>();
        if (!vm.count("cache-static-root"))
            throw std::runtime_error("'--cache-static-root' must be explicity given when using a static cache");
    }
    if (!_cache_static_path.empty() && !fs::is_directory(_cache_static_path))
        throw std::runtime_error(
            util::str("No such directory: ", _cache_static_path));

    if (vm.count("local-domain")) {
        auto tld_rx = boost::regex("[-0-9a-zA-Z]+");
        auto local_domain = vm["local-domain"].as<string>();
        if (!boost::regex_match(local_domain, tld_rx)) {
            throw std::runtime_error(util::str(
                    "Invalid TLD for '--local-domain': ", local_domain));
        }
        _local_domain = boost::algorithm::to_lower_copy(local_domain);
    }

    if (vm.count("origin-doh-base")) {
        auto doh_base = vm["origin-doh-base"].as<string>();
        _origin_doh_endpoint = doh::endpoint_from_base(doh_base);
        if (!_origin_doh_endpoint)
            throw std::runtime_error(util::str(
                    "Invalid URL for '--origin-doh-base': ", doh_base));
    }
}

#undef _DEFAULT_STATIC_CACHE_SUBDIR
} // ouinet namespace
