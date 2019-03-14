#pragma once

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include "namespaces.h"
#include "util.h"
#include "util/crypto.h"
#include "cache/index.h"
#include "increase_open_file_limit.h"
#include "endpoint.h"

namespace ouinet {

class ClientConfig {
public:
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

    void set_injector_endpoint(const Endpoint& ep);

    const std::string& tls_injector_cert_path() const {
        return _tls_injector_cert_path;
    }

    const std::string& tls_ca_cert_store_path() const {
        return _tls_ca_cert_store_path;
    }

    const asio::ip::tcp::endpoint& local_endpoint() const {
        return _local_ep;
    }

    const std::string& index_ipns_id() const {
        return _index_ipns_id;
    }

    void set_index_ipns_id(std::string ipns_id) {
        _index_ipns_id = std::move(ipns_id);
    }

    boost::posix_time::time_duration max_cached_age() const {
        return _max_cached_age;
    }

    boost::optional<std::string>
    credentials_for(const std::string& injector) const {
        auto i = _injector_credentials.find(injector);
        if (i == _injector_credentials.end()) return {};
        return i->second;
    }

    void set_credentials( const std::string& injector
                        , const std::string& cred) {
        _injector_credentials[injector] = cred;
    }

    bool enable_http_connect_requests() const {
        return _enable_http_connect_requests;
    }

    asio::ip::tcp::endpoint front_end_endpoint() const {
        return _front_end_endpoint;
    }

    boost::optional<util::Ed25519PublicKey> index_bep44_pub_key() const {
        return _index_bep44_pubkey;
    }

    unsigned int index_bep44_capacity() const {
        return _index_bep44_capacity;
    }

    bool is_help() const { return _is_help; }

    boost::program_options::options_description description()
    {
        using namespace std;
        namespace po = boost::program_options;

        po::options_description desc;

        desc.add_options()
           ("help", "Produce this help message")
           ("repo", po::value<string>(), "Path to the repository root")

           // Client options
           ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen")
           ("front-end-ep"
            , po::value<string>()
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
              "where <TYPE> can be \"tcp\", \"obfs2\", \"obfs3\", \"obfs4\" or \"i2p\", "
              "and <EP> depends on the type of endpoint: "
              "<IP>:<PORT> for TCP (and TLS), <IP>:<PORT>[,<OPTION>=<VALUE>...] for OBFS, "
              "<B32_PUBKEY>.b32.i2p or <B64_PUBKEY> for I2P")
           ("injector-credentials", po::value<string>()
            , "<username>:<password> authentication pair for the injector")
           ("injector-tls-cert-file", po::value<string>(&_tls_injector_cert_path)
            , "Path to the Injector's TLS certificate")

           // Cache options
           ("disable-cache", "Disable all cache operations (even initialization)")
           ("cache-index"
            , po::value<string>()->default_value("bep44")
            , "Cache index to use, can be either \"bep44\" or \"btree\"")
           ("index-ipns-id"
            , po::value<string>()->default_value("")
            , "Index ID for the IPFS IPNS subsystem")
           ("index-bep44-public-key"
            , po::value<string>()
            , "Index public key for the BitTorrent BEP44 subsystem")
           ("index-bep44-capacity"
            , po::value<unsigned int>()->default_value(_index_bep44_capacity)
            , "Maximum number of entries to be kept (and persisted) in the BEP44 index")
           ("max-cached-age"
            , po::value<int>()->default_value(_max_cached_age.total_seconds())
            , "Discard cached content older than this many seconds "
              "(0: discard all; -1: discard none)")

           // Request routing options
           ("disable-origin-access", po::bool_switch(&_disable_origin_access)->default_value(false)
            , "Disable direct access to the origin (forces use of injector and the cache)")
           ("disable-proxy-access", po::bool_switch(&_disable_proxy_access)->default_value(false)
            , "Disable proxied access to the origin (via the injector)")
           ("local-domain"
            , po::value<string>()->default_value("local")
            , "Always use origin access and never use cache for this TLD")
           ("enable-http-connect-requests", po::bool_switch(&_enable_http_connect_requests)
            , "Enable HTTP CONNECT requests")
           ;

        return desc;
    }

    IndexType cache_index_type() const { return _cache_index_type; }

    bool cache_enabled() const { return !_disable_cache; }

    bool is_origin_access_enabled() const { return !_disable_origin_access; }
    void is_origin_access_enabled(bool v) { _disable_origin_access = !v; }

    bool is_proxy_access_enabled() const { return !_disable_proxy_access; }
    void is_proxy_access_enabled(bool v) { _disable_proxy_access = !v; }

    std::string local_domain() const { return _local_domain; }

private:
    bool _is_help = false;
    fs::path _repo_root;
    fs::path _ouinet_conf_file = "ouinet-client.conf";
    asio::ip::tcp::endpoint _local_ep;
    boost::optional<Endpoint> _injector_ep;
    std::string _tls_injector_cert_path;
    std::string _tls_ca_cert_store_path;
    std::string _index_ipns_id;
    bool _enable_http_connect_requests = false;
    bool _disable_origin_access = false;
    bool _disable_proxy_access = false;
    asio::ip::tcp::endpoint _front_end_endpoint;
    IndexType _cache_index_type = IndexType::btree;

    boost::posix_time::time_duration _max_cached_age
        = boost::posix_time::hours(7*24);  // one week

    std::map<std::string, std::string> _injector_credentials;

    boost::optional<util::Ed25519PublicKey> _index_bep44_pubkey;
    unsigned int _index_bep44_capacity = 1000;
    bool _disable_cache = false;
    std::string _local_domain;
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
                util::str("The 'repo' argument is missing\n", desc, "\n"));
    }

    _repo_root = fs::path(vm["repo"].as<string>());

    if (!fs::exists(_repo_root)) {
        throw std::runtime_error(
                util::str("Directory ", _repo_root, " does not exist.\n"
                         , desc, "\n"));
    }

    if (!fs::is_directory(_repo_root)) {
        throw std::runtime_error(
                util::str("The path ", _repo_root, " is not a directory.\n"
                         , desc, "\n"));
    }

    fs::path ouinet_conf_path = _repo_root/_ouinet_conf_file;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        throw std::runtime_error(
                util::str("The path ", _repo_root, " does not contain the "
                         , _ouinet_conf_file, " configuration file.\n"
                         , desc));
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (vm.count("max-cached-age")) {
        _max_cached_age = boost::posix_time::seconds(vm["max-cached-age"].as<int>());
    }

    if (!vm.count("listen-on-tcp")) {
        throw std::runtime_error(
                util::str( "The parameter 'listen-on-tcp' is missing.\n"
                         , desc, "\n"));
    }

    _local_ep = util::parse_tcp_endpoint(vm["listen-on-tcp"].as<string>());

    if (vm.count("injector-ep")) {
        auto injector_ep_str = vm["injector-ep"].as<string>();

        if (!injector_ep_str.empty()) {
            auto opt = parse_endpoint(injector_ep_str);

            if (!opt) {
                throw std::runtime_error( "Failed to parse endpoint \""
                        + injector_ep_str + "\"");
            }

            _injector_ep = *opt;
        }
    }

    if (vm.count("front-end-ep")) {
        auto ep_str = vm["front-end-ep"].as<string>();

        if (!ep_str.empty()) {
            sys::error_code ec;
            _front_end_endpoint = util::parse_tcp_endpoint(ep_str, ec);

            if (ec) {
                throw std::runtime_error( "Failed to parse endpoint \""
                        + ep_str + "\"");
            }
        }
    }

    if (vm.count("index-ipns-id")) {
        _index_ipns_id = vm["index-ipns-id"].as<string>();
    }

    if (vm.count("injector-credentials")) {
        auto cred = vm["injector-credentials"].as<string>();

        if (!cred.empty()
          && cred.find(':') == string::npos) {
            throw std::runtime_error(util::str(
                "The '--injector-credentials' argument expects a string "
                "in the format <username>:<password>. But the provided "
                "string \"", cred, "\" is missing a colon."));
        }

        if (!_injector_ep) {
            throw std::runtime_error(util::str(
                "The '--injector-credentials' argument must be used with "
                "'--injector-ep'"));
        }

        set_credentials(util::str(*_injector_ep), cred);
    }

    if (vm.count("index-bep44-public-key")) {
        string value = vm["index-bep44-public-key"].as<string>();

        _index_bep44_pubkey = util::Ed25519PublicKey::from_hex(value);

        if (!_index_bep44_pubkey) {
            throw std::runtime_error(
                    util::str("Failed parsing '", value, "' as Ed25519 public key"));
        }
    }

    if (vm.count("index-bep44-capacity")) {
        _index_bep44_capacity = vm["index-bep44-capacity"].as<unsigned int>();
    }

    if (vm.count("cache-index")) {
        auto type = vm["cache-index"].as<string>();

        if (type == "btree") {
            _cache_index_type = IndexType::btree;
        }
        else if (type == "bep44") {
            _cache_index_type = IndexType::bep44;
        }
        else {
            throw std::runtime_error("Invalid value for --cache-index");
        }
    }

    if (vm.count("disable-cache")) {
        _disable_cache = true;
    }

    if (!_disable_cache && _cache_index_type == IndexType::bep44 && !_index_bep44_pubkey) {
        throw std::runtime_error("BEP44 index selected but no injector BEP44 public key specified");
    }

    if (vm.count("local-domain")) {
        auto tld_rx = boost::regex("[-0-9a-zA-Z]+");
        auto local_domain = vm["local-domain"].as<string>();
        if (!boost::regex_match(local_domain, tld_rx)) {
            throw std::runtime_error("Invalid TLD for --local-domain");
        }
        _local_domain = boost::algorithm::to_lower_copy(local_domain);
    }
}

inline
void ClientConfig::set_injector_endpoint(const Endpoint& ep)
{
    _injector_ep = ep;
}

} // ouinet namespace
