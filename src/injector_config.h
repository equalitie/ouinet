#pragma once

#include <boost/asio/ip/udp.hpp>
#include "util/crypto.h"

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

    boost::optional<std::string> bep5_injector_swarm_name() const
    {
        return _bep5_injector_swarm_name;
    }

    boost::optional<asio::ip::udp::endpoint> bittorrent_endpoint() const
    {
        if (_bt_endpoint) return _bt_endpoint;
        if (_utp_tls_endpoint) return _utp_tls_endpoint;
        if (_utp_endpoint) return _utp_endpoint;
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

    const std::string& tls_ca_cert_store_path() const
    { return _tls_ca_cert_store_path; }

    util::Ed25519PrivateKey index_bep44_private_key() const
    { return _index_bep44_private_key; }

    unsigned int index_bep44_capacity() const
    { return _index_bep44_capacity; }

    unsigned int cache_local_capacity() const
    { return _cache_local_capacity; }

    bool cache_enabled() const { return !_disable_cache; }

private:
    void setup_index_bep44_private_key(const std::string& hex);

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
    boost::optional<asio::ip::udp::endpoint> _bt_endpoint;
    boost::optional<std::string> _bep5_injector_swarm_name;
    boost::filesystem::path OUINET_CONF_FILE = "ouinet-injector.conf";
    std::string _credentials;
    util::Ed25519PrivateKey _index_bep44_private_key;
    unsigned int _index_bep44_capacity;
    unsigned int _cache_local_capacity;
    bool _disable_cache = false;
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
        ("listen-in-bep5-swarm", po::value<string>(), "Bep5 swarm name to announce our WAN IP")
        ("credentials", po::value<string>()
         , "<username>:<password> authentication pair. "
           "If unused, this injector shall behave as an open proxy.")

        ("tls-ca-cert-store-path", po::value<string>(&_tls_ca_cert_store_path)
         , "Path to the CA certificate store file")
        // Cache options
        ("disable-cache", "Disable all cache operations (even initialization)")
        ("seed-content"
         , po::value<bool>()->default_value(false)
         , "Seed the content instead of only signing it")
        ("cache-local-capacity"
         , po::value<unsigned int>()->default_value(10000)  // arbitrarily chosen
         , "Maximum number of resources to be cached locally")
        ("index-bep44-private-key", po::value<string>()
         , "Index private key for the BitTorrent BEP44 subsystem")
        // By default, it is not desirable that the injector actively republishes BEP44 entries.
        // If a client caused a new injection of a URL (whether there was an existing injection of it or not),
        // and the client goes immediately offline (so that its IPFS data is no longer available),
        // we prefer that the newly inserted BEP44 entries fade away as fast as possible,
        // so that they either disappear or are eventually replaced by others being actively seeded by clients.
        // Better have stale content or no trace of the content at all,
        // than index entries that keep clients stuck for some minutes trying to fetch unavailable data.
        // A positive (and big) value may make sense for an injector that
        // kept content for a long time or indefinitely
        // (e.g. if IPFS' urlstore may be used in the future).
        ("index-bep44-capacity"
         , po::value<unsigned int>()->default_value(0)
         , "Maximum number of entries to be kept (and persisted) in the BEP44 index")
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
        throw std::runtime_error("The 'repo' argument is missing");
    }

    _repo_root = vm["repo"].as<string>();

    if (!exists(_repo_root) || !is_directory(_repo_root)) {
        throw std::runtime_error(util::str(
            "The path ", _repo_root, " either doesn't exist or"
            " isn't a directory."));
    }

    fs::path ouinet_conf_path = _repo_root/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        throw std::runtime_error(util::str(
            "The path ", _repo_root, " does not contain the "
            , OUINET_CONF_FILE, " configuration file."));
    }

    std::ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        _open_file_limit = vm["open-file-limit"].as<unsigned int>();
    }

    if (vm.count("credentials")) {
        _credentials = vm["credentials"].as<string>();
        if (!_credentials.empty() && _credentials.find(':') == string::npos) {
            throw std::runtime_error(util::str(
                "The '--credentials' argument expects a string "
                "in the format <username>:<password>. But the provided "
                "string \"", _credentials, "\" is missing a colon."));
        }
    }

    // Unfortunately, Boost.ProgramOptions doesn't support arguments without
    // values in config files. Thus we need to force the 'listen-on-i2p' arg
    // to have one of the strings values "true" or "false".
    if (vm.count("listen-on-i2p")) {
        auto value = vm["listen-on-i2p"].as<string>();

        if (value != "" && value != "true" && value != "false") {
            throw std::runtime_error(
                "The listen-on-i2p parameter may be either 'true' or 'false'");
        }

        _listen_on_i2p = (value == "true");
    }

    if (vm.count("listen-on-tcp")) {
        _tcp_endpoint = util::parse_tcp_endpoint(vm["listen-on-tcp"].as<string>());
    }

    if (vm.count("listen-on-tcp-tls")) {
        _tcp_tls_endpoint = util::parse_tcp_endpoint(vm["listen-on-tcp-tls"].as<string>());
    }

    if (vm.count("listen-on-utp")) {
        sys::error_code ec;
        auto ep = util::parse_endpoint<asio::ip::udp>(vm["listen-on-utp"].as<string>(), ec);
        if (ec) throw std::runtime_error("Failed to parse utp endpoint");
        _utp_endpoint = ep;
    }

    if (vm.count("listen-on-utp-tls")) {
        sys::error_code ec;
        auto ep = util::parse_endpoint<asio::ip::udp>(vm["listen-on-utp-tls"].as<string>(), ec);
        if (ec) throw std::runtime_error("Failed to parse utp endpoint");
        _utp_tls_endpoint = ep;
    }

    if (vm.count("listen-on-lampshade")) {
        _lampshade_endpoint = util::parse_tcp_endpoint(vm["listen-on-lampshade"].as<string>());
    }

    if (vm.count("listen-on-obfs2")) {
        _obfs2_endpoint = util::parse_tcp_endpoint(vm["listen-on-obfs2"].as<string>());
    }

    if (vm.count("listen-on-obfs3")) {
        _obfs3_endpoint = util::parse_tcp_endpoint(vm["listen-on-obfs3"].as<string>());
    }

    if (vm.count("listen-on-obfs4")) {
        _obfs4_endpoint = util::parse_tcp_endpoint(vm["listen-on-obfs4"].as<string>());
    }

    if (vm.count("listen-in-bep5-swarm")) {
        _bep5_injector_swarm_name = vm["listen-in-bep5-swarm"].as<string>();
    }

    setup_index_bep44_private_key( vm.count("index-bep44-private-key")
                                 ? vm["index-bep44-private-key"].as<string>()
                                 : string());

    if (vm.count("index-bep44-capacity")) {
        _index_bep44_capacity = vm["index-bep44-capacity"].as<unsigned int>();
    }
    if (vm.count("cache-local-capacity")) {
        _cache_local_capacity = vm["cache-local-capacity"].as<unsigned int>();
    }

    if (vm.count("disable-cache")) {
        _disable_cache = true;
    }
}

inline void InjectorConfig::setup_index_bep44_private_key(const std::string& hex)
{
    fs::path priv_config = _repo_root/"bep44-private-key";
    fs::path pub_config  = _repo_root/"bep44-public-key";

    if (hex.empty()) {
        if (fs::exists(priv_config)) {
            fs::ifstream(priv_config) >> _index_bep44_private_key;
            fs::ofstream(pub_config)  << _index_bep44_private_key.public_key();
            return;
        }

        _index_bep44_private_key = util::Ed25519PrivateKey::generate();

        fs::ofstream(priv_config) << _index_bep44_private_key;
        fs::ofstream(pub_config)  << _index_bep44_private_key.public_key();
        return;
    }

    _index_bep44_private_key = *util::Ed25519PrivateKey::from_hex(hex);
    fs::ofstream(priv_config) << _index_bep44_private_key;
    fs::ofstream(pub_config)  << _index_bep44_private_key.public_key();
}

} // ouinet namespace
