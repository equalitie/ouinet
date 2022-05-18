#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/regex.hpp>

#include "logger.h"
#include "util/crypto.h"
#include "parse/endpoint.h"
#include "bep5_swarms.h"

namespace ouinet {

class InjectorConfig {
public:
    InjectorConfig() = default;
    InjectorConfig(const InjectorConfig&) = default;
    InjectorConfig(InjectorConfig&&) = default;
    InjectorConfig& operator=(const InjectorConfig&) = default;
    InjectorConfig& operator=(InjectorConfig&&) = default;

    // May thow on error.
    InjectorConfig(int argc, const char** argv);

    bool is_help() const
    { return _is_help; }

    boost::optional<size_t> open_file_limit() const
    { return _open_file_limit; }

    boost::filesystem::path repo_root() const
    { return _repo_root; }

    bool listen_on_i2p() const
    { return _listen_on_i2p; }

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

    boost::optional<asio::ip::tcp::endpoint> lampshade_endpoint() const
    { return _lampshade_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs2_endpoint() const
    { return _obfs2_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs3_endpoint() const
    { return _obfs3_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs4_endpoint() const
    { return _obfs4_endpoint; }

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
    void setup_ed25519_private_key(const std::string& hex);

private:
    bool _is_help = false;
    boost::filesystem::path _repo_root;
    boost::optional<size_t> _open_file_limit;
    bool _listen_on_i2p = false;
    std::string _tls_ca_cert_store_path;
    boost::optional<asio::ip::tcp::endpoint> _tcp_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _tcp_tls_endpoint;
    boost::optional<asio::ip::udp::endpoint> _utp_endpoint;
    boost::optional<asio::ip::udp::endpoint> _utp_tls_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _lampshade_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs2_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs3_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs4_endpoint;
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

    po::options_description desc("\nOptions");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("log-level", po::value<string>()->default_value("INFO"), "Set log level: silly, debug, verbose, info, warn, error, abort")

        // Injector options
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")

        // Transport options
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen (cleartext)")
        ("listen-on-tcp-tls", po::value<string>(), "IP:PORT endpoint on which we'll listen (encrypted)")
        ("listen-on-utp", po::value<string>(), "IP:PORT UDP endpoint on which we'll listen (cleartext)")
        ("listen-on-utp-tls", po::value<string>(), "IP:PORT UDP endpoint on which we'll listen (encrypted)")
        ("listen-on-lampshade", po::value<string>(), "IP:PORT endpoint on which we'll listen using the lampshade pluggable transport")
        ("listen-on-obfs2", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs2 pluggable transport")
        ("listen-on-obfs3", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs3 pluggable transport")
        ("listen-on-obfs4", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs4 pluggable transport")
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
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
        if (!log_level(level))
            throw std::runtime_error(util::str("Invalid log level: ", level));
        LOG_INFO("Log level set to: ", level);
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
            fs::ifstream(priv_config) >> _ed25519_private_key;
            fs::ofstream(pub_config)  << _ed25519_private_key.public_key();
            return;
        }

        _ed25519_private_key = util::Ed25519PrivateKey::generate();

        fs::ofstream(priv_config) << _ed25519_private_key;
        fs::ofstream(pub_config)  << _ed25519_private_key.public_key();
        return;
    }

    _ed25519_private_key = *util::Ed25519PrivateKey::from_hex(hex);
    fs::ofstream(priv_config) << _ed25519_private_key;
    fs::ofstream(pub_config)  << _ed25519_private_key.public_key();
}

} // ouinet namespace
