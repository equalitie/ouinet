#pragma once

#include "util/crypto.h"
#include "cache/index.h"

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

    boost::optional<asio::ip::tcp::endpoint> tcp_endpoint() const
    { return _tcp_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> tls_endpoint() const
    { return _tls_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs2_endpoint() const
    { return _obfs2_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs3_endpoint() const
    { return _obfs3_endpoint; }

    boost::optional<asio::ip::tcp::endpoint> obfs4_endpoint() const
    { return _obfs4_endpoint; }

    static boost::program_options::options_description
    options_description();

    std::string credentials() const
    { return _credentials; }

    util::Ed25519PrivateKey index_bep44_private_key() const
    { return _index_bep44_private_key; }

    unsigned int index_bep44_capacity() const
    { return _index_bep44_capacity; }

    IndexType cache_index_type() const
    { return _cache_index_type; }

    bool cache_enabled() const { return !_disable_cache; }

    bool seed_content() const { return _seed_content; }

private:
    void setup_index_bep44_private_key(const std::string& hex);

private:
    bool _is_help = false;
    boost::filesystem::path _repo_root;
    boost::optional<size_t> _open_file_limit;
    bool _listen_on_i2p = false;
    boost::optional<asio::ip::tcp::endpoint> _tcp_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _tls_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs2_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs3_endpoint;
    boost::optional<asio::ip::tcp::endpoint> _obfs4_endpoint;
    boost::filesystem::path OUINET_CONF_FILE = "ouinet-injector.conf";
    std::string _credentials;
    util::Ed25519PrivateKey _index_bep44_private_key;
    unsigned int _index_bep44_capacity;
    IndexType _cache_index_type = IndexType::btree;
    bool _disable_cache = false;
    bool _seed_content = false;
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
        ("listen-on-tls", po::value<string>(), "IP:PORT endpoint on which we'll listen (encrypted)")
        ("listen-on-obfs2", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs2 pluggable transport")
        ("listen-on-obfs3", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs3 pluggable transport")
        ("listen-on-obfs4", po::value<string>(), "IP:PORT endpoint on which we'll listen using the obfs4 pluggable transport")
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
        ("credentials", po::value<string>()
         , "<username>:<password> authentication pair. "
           "If unused, this injector shall behave as an open proxy.")

        // Cache options
        ("disable-cache", "Disable all cache operations (even initialization)")
        ("seed-content"
         , po::value<bool>()->default_value(false)
         , "Seed the content instead of only signing it")
        ("cache-index"
         , po::value<string>()->default_value("btree")
         , "Cache index to use, can be either \"btree\" or \"bep44\"")
        ("index-bep44-private-key", po::value<string>()
         , "Index private key for the BitTorrent BEP44 subsystem")
        ("index-bep44-capacity"
         , po::value<unsigned int>()->default_value(1000)  // arbitrarily chosen default
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

    if (vm.count("listen-on-tls")) {
        _tls_endpoint = util::parse_tcp_endpoint(vm["listen-on-tls"].as<string>());
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

    setup_index_bep44_private_key( vm.count("index-bep44-private-key")
                                 ? vm["index-bep44-private-key"].as<string>()
                                 : string());

    if (vm.count("index-bep44-capacity")) {
        _index_bep44_capacity = vm["index-bep44-capacity"].as<unsigned int>();
    }

    if (vm.count("seed-content")) {
        _seed_content = vm["seed-content"].as<bool>();
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
