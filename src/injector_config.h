#pragma once

#include <set>
#include <vector>

#include <boost/asio/ip/udp.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/regex.hpp>

#include "logger.h"
#include "http_logger.h"
#include "util/crypto.h"
#include "parse/endpoint.h"
#include "bep5_swarms.h"
#include "bittorrent/bootstrap.h"

namespace ouinet {

#define _HTTP_LOG_FILE_NAME "access.log"
static const fs::path http_log_file_name{_HTTP_LOG_FILE_NAME};

class InjectorConfig {
public:
    using ExtraBtBsServers = std::set<bittorrent::bootstrap::Address>;

    InjectorConfig() = default;
    InjectorConfig(const InjectorConfig&) = default;
    InjectorConfig(InjectorConfig&&) = default;
    InjectorConfig& operator=(const InjectorConfig&) = default;
    InjectorConfig& operator=(InjectorConfig&&) = default;

    // May thow on error.
    InjectorConfig(int argc, const char** argv);

    bool is_help() const
    { return _is_help; }

    const ExtraBtBsServers& bt_bootstrap_extras() const {
        return _bt_bootstrap_extras;
    }

    boost::optional<size_t> open_file_limit() const
    { return _open_file_limit; }

    boost::filesystem::path repo_root() const
    { return _repo_root; }

    inline bool _is_http_log_file_enabled() const {
        return http_logger.get_log_file() != nullptr;
    }

    inline void _is_http_log_file_enabled(bool v) {
        if (!v) {
            http_logger.log_to_file("");
            return;
        }

        if (_is_http_log_file_enabled()) return;

        auto current_log_path = http_logger.current_log_file();
        auto http_log_path = current_log_path.empty()
                               ? (_repo_root / http_log_file_name).string()
                               : current_log_path;

        http_logger.log_to_file(http_log_path);
        LOG_INFO("Log file set to: ", http_log_path);
    }

#ifdef __EXPERIMENTAL__
    bool listen_on_i2p() const
    { return _listen_on_i2p; }
#endif // ifdef __EXPERIMENTAL__

    std::string bep5_injector_swarm_name() const
    {
        return _bep5_injector_swarm_name;
    }

    asio::ip::udp::endpoint bittorrent_endpoint() const
    {
        if (_utp_tls_endpoint) return *_utp_tls_endpoint;
        if (_utp_endpoint) return *_utp_endpoint;
        return asio::ip::udp::endpoint(asio::ip::address_v4::any(), 4567);
    }

    boost::optional<asio::ip::tcp::endpoint> tcp_endpoint() const
    { return _tcp_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> tcp_tls_endpoint() const
    { return _tcp_tls_endpoint; }

    boost::optional<asio::ip::udp::endpoint> utp_endpoint() const
    { return _utp_endpoint; }

    boost::optional<asio::ip::udp::endpoint> utp_tls_endpoint() const
    { return _utp_tls_endpoint; }

#ifdef __EXPERIMENTAL__
    boost::optional<asio::ip::tcp::endpoint> lampshade_endpoint() const
    { return _lampshade_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs2_endpoint() const
    { return _obfs2_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs3_endpoint() const
    { return _obfs3_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs4_endpoint() const
    { return _obfs4_endpoint; }
#endif // ifdef __EXPERIMENTAL__

    boost::program_options::options_description
    options_description();

    std::string credentials() const
    { return _credentials; }

    bool is_proxy_enabled() const
    { return !_disable_proxy; }

    boost::optional<boost::regex> target_rx() const
    { return _target_rx; }

    const std::string& tls_ca_cert_store_path() const
    { return _tls_ca_cert_store_path; }

    util::Ed25519PrivateKey cache_private_key() const
    { return _ed25519_private_key; }

private:
    void setup_ed25519_private_key(const std::string& hex);

private:
    bool _is_help = false;
    boost::filesystem::path _repo_root;
    ExtraBtBsServers _bt_bootstrap_extras;
    boost::optional<size_t> _open_file_limit;
#ifdef __EXPERIMENTAL__
    bool _listen_on_i2p = false;
#endif // ifdef __EXPERIMENTAL__
    std::string _tls_ca_cert_store_path;
    boost::optional<asio::ip::tcp::endpoint> _tcp_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _tcp_tls_endpoint;
    boost::optional<asio::ip::udp::endpoint> _utp_endpoint;
    boost::optional<asio::ip::udp::endpoint> _utp_tls_endpoint;
#ifdef __EXPERIMENTAL__
    boost::optional<asio::ip::tcp::endpoint> _lampshade_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs2_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs3_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs4_endpoint;
#endif // ifdef __EXPERIMENTAL__
    std::string _bep5_injector_swarm_name;
    boost::filesystem::path OUINET_CONF_FILE = "ouinet-injector.conf";
    std::string _credentials;
    bool _disable_proxy = false;
    boost::optional<boost::regex> _target_rx;
    util::Ed25519PrivateKey _ed25519_private_key;
};

inline
boost::program_options::options_description
InjectorConfig::options_description()
{
    namespace po = boost::program_options;
    using std::string;

    po::options_description desc("Options");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("log-level", po::value<string>()->default_value(util::str(default_log_level()))
         , "Set log level: silly, debug, verbose, info, warn, error, abort")
        ("enable-http-log-file"
         , po::bool_switch()->default_value(false)
         , "Enable logging of HTTP requests received via public mode"
           " to log file \"" _HTTP_LOG_FILE_NAME "\" under the repository root")
        ("bt-bootstrap-extra", po::value<std::vector<string>>()->composing()
         , "Extra BitTorrent bootstrap server (in <HOST> or <HOST>:<PORT> format) "
           "to start the DHT (can be used several times). "
           "<HOST> can be a host name, <IPv4> address, or <[IPv6]> address. "
           "This option is persistent.")

        // Injector options
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")

        // Transport options
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen (cleartext)")
        ("listen-on-tcp-tls", po::value<string>(), "IP:PORT endpoint on which we'll listen (encrypted)")
        ("listen-on-utp", po::value<string>(), "IP:PORT UDP endpoint on which we'll listen (cleartext)")
        ("listen-on-utp-tls", po::value<string>(), "IP:PORT UDP endpoint on which we'll listen (encrypted)")
#ifdef __EXPERIMENTAL__
        ("listen-on-lampshade", po::value<string>(), "IP:PORT endpoint on which we'll listen using the lampshade pluggable transport")
        ("listen-on-obfs2", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs2 pluggable transport")
        ("listen-on-obfs3", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs3 pluggable transport")
        ("listen-on-obfs4", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs4 pluggable transport")
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
#endif // ifdef __EXPERIMENTAL__
        // It always announces the TLS uTP endpoint since
        // a TLS certificate is always generated.
        ("credentials", po::value<string>()
         , "<username>:<password> authentication pair. "
           "If unused, this injector shall behave as an open proxy.")
        ("disable-proxy", po::bool_switch(&_disable_proxy)->default_value(false)
         , "Reject plain HTTP proxy requests (including CONNECT for HTTPS)")
        ("restricted", po::value<string>()
         , "Only allow injection of URIs fully matching the given regular expression. "
           "This option implies \"--disable-proxy\". "
           "Example: https?://(www\\.)?(example\\.com|test\\.net/foo)/.*")

        ("tls-ca-cert-store-path", po::value<string>(&_tls_ca_cert_store_path)
         , "Path to the CA certificate store file")
        // Cache options
        ("ed25519-private-key", po::value<string>()
         , "Ed25519 private key for cache-related signatures (hex-encoded)")
        ;

    return desc;
}

inline
InjectorConfig::InjectorConfig(int argc, const char**argv)
{
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    using std::string;

    auto desc = options_description();

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        _is_help = true;
        return;
    }

    if (!vm.count("repo")) {
        throw std::runtime_error("The '--repo' option is missing");
    }

    _repo_root = vm["repo"].as<string>();

    if (!fs::exists(_repo_root)) {
        throw std::runtime_error(
                util::str("No such directory: ", _repo_root));
    }

    if (!fs::is_directory(_repo_root)) {
        throw std::runtime_error(
                util::str("The path is not a directory: ", _repo_root));
    }

    {
        fs::path ouinet_conf_path = _repo_root/OUINET_CONF_FILE;
        if (!fs::is_regular_file(ouinet_conf_path)) {
            throw std::runtime_error(util::str(
                "The path ", _repo_root, " does not contain the "
                , OUINET_CONF_FILE, " configuration file"));
        }
        std::ifstream ouinet_conf(ouinet_conf_path.native());
        po::store(po::parse_config_file(ouinet_conf, desc), vm);
        po::notify(vm);
    }

    if (vm.count("log-level")) {
        auto level = boost::algorithm::to_upper_copy(vm["log-level"].as<string>());
        auto ll_o = log_level_from_string(level);
        if (!ll_o)
            throw std::runtime_error(util::str("Invalid log level: ", level));
        logger.set_threshold(*ll_o);
        LOG_INFO("Log level set to: ", level);
    }

    if (vm["enable-http-log-file"].as<bool>()) {
        _is_http_log_file_enabled(true);
    }

    if (vm.count("bt-bootstrap-extra")) {
        for (const auto& btbsx : vm["bt-bootstrap-extra"].as<std::vector<string>>()) {
            // Better processing will take place later on, just very basic checking here.
            auto btbs_addr = bittorrent::bootstrap::parse_address(btbsx);
            if (!btbs_addr)
                throw std::runtime_error(util::str("Invalid BitTorrent bootstrap server: ", btbsx));
            _bt_bootstrap_extras.insert(*btbs_addr);
        }
    }

    if (vm.count("open-file-limit")) {
        _open_file_limit = vm["open-file-limit"].as<unsigned int>();
    }

    if (vm.count("credentials")) {
        _credentials = vm["credentials"].as<string>();
        if (!_credentials.empty() && _credentials.find(':') == string::npos) {
            throw std::runtime_error(util::str(
                "The '--credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", _credentials));
        }
    }

    if (vm.count("restricted")) {
        _target_rx = boost::regex{vm["restricted"].as<string>()};
        _disable_proxy = true;
    }

#ifdef __EXPERIMENTAL__
    // Unfortunately, Boost.ProgramOptions doesn't support arguments without
    // values in config files. Thus we need to force the 'listen-on-i2p' arg
    // to have one of the strings values "true" or "false".
    if (vm.count("listen-on-i2p")) {
        auto value = vm["listen-on-i2p"].as<string>();

        if (value != "" && value != "true" && value != "false") {
            throw std::runtime_error(
                "The '--listen-on-i2p' argument may be either 'true' or 'false'");
        }

        _listen_on_i2p = (value == "true");
    }
#endif // ifdef __EXPERIMENTAL__

    if (vm.count("listen-on-tcp")) {
        auto opt_tcp_endpoint = parse::endpoint<asio::ip::tcp>(vm["listen-on-tcp"].as<string>());
        if (!opt_tcp_endpoint) {
            throw std::runtime_error("Failed to parse '--listen-on-tcp' argument");
        }
        _tcp_endpoint = *opt_tcp_endpoint;
    }

    if (vm.count("listen-on-tcp-tls")) {
        auto opt_tcp_tls_endpoint = parse::endpoint<asio::ip::tcp>(vm["listen-on-tcp-tls"].as<string>());
        if (!opt_tcp_tls_endpoint) {
            throw std::runtime_error("Failed to parse '--listen-on-tcp-tls' argument");
        }
        _tcp_tls_endpoint = *opt_tcp_tls_endpoint;
    }

    if (vm.count("listen-on-utp")) {
        sys::error_code ec;
        auto ep = parse::endpoint<asio::ip::udp>(vm["listen-on-utp"].as<string>(), ec);
        if (ec) throw std::runtime_error("Failed to parse uTP endpoint");
        _utp_endpoint = ep;
    }

    if (vm.count("listen-on-utp-tls")) {
        sys::error_code ec;
        auto ep = parse::endpoint<asio::ip::udp>(vm["listen-on-utp-tls"].as<string>(), ec);
        if (ec) throw std::runtime_error("Failed to parse uTP endpoint");
        _utp_tls_endpoint = ep;
    }

#ifdef __EXPERIMENTAL__
    if (vm.count("listen-on-lampshade")) {
        _lampshade_endpoint = *parse::endpoint<asio::ip::tcp>(vm["listen-on-lampshade"].as<string>());
    }

    if (vm.count("listen-on-obfs2")) {
        _obfs2_endpoint = *parse::endpoint<asio::ip::tcp>(vm["listen-on-obfs2"].as<string>());
    }

    if (vm.count("listen-on-obfs3")) {
        _obfs3_endpoint = *parse::endpoint<asio::ip::tcp>(vm["listen-on-obfs3"].as<string>());
    }

    if (vm.count("listen-on-obfs4")) {
        _obfs4_endpoint = *parse::endpoint<asio::ip::tcp>(vm["listen-on-obfs4"].as<string>());
    }
#endif // ifdef __EXPERIMENTAL__

    // Please note that generating keys takes a long time
    // and it may cause time outs in CI tests.
    setup_ed25519_private_key( vm.count("ed25519-private-key")
                             ? vm["ed25519-private-key"].as<string>()
                             : string());

    // https://redmine.equalit.ie/issues/14920#note-1
    _bep5_injector_swarm_name
        = bep5::compute_injector_swarm_name( _ed25519_private_key.public_key()
                                           , http_::protocol_version_current);
}

inline void InjectorConfig::setup_ed25519_private_key(const std::string& hex)
{
    fs::path priv_config = _repo_root/"ed25519-private-key";
    fs::path pub_config  = _repo_root/"ed25519-public-key";

    if (hex.empty()) {
        if (fs::exists(priv_config)) {
            boost::nowide::ifstream(priv_config) >> _ed25519_private_key;
            boost::nowide::ofstream(pub_config)  << _ed25519_private_key.public_key();
            return;
        }

        _ed25519_private_key = util::Ed25519PrivateKey::generate();

        boost::nowide::ofstream(priv_config) << _ed25519_private_key;
        boost::nowide::ofstream(pub_config)  << _ed25519_private_key.public_key();
        return;
    }

    _ed25519_private_key = *util::Ed25519PrivateKey::from_hex(hex);
    boost::nowide::ofstream(priv_config) << _ed25519_private_key;
    boost::nowide::ofstream(pub_config)  << _ed25519_private_key.public_key();
}

} // ouinet namespace
