#include <boost/optional/optional_io.hpp>
#include "client_config.h"

namespace ouinet {

template<class... Args>
inline
std::runtime_error error(Args&&... args) {
    return std::runtime_error(util::str(std::forward<Args>(args)...));
}

// Helper to avoid writing the name of the option twice.
template<typename T>
static boost::optional<T> as_optional(const boost::program_options::variables_map& vm, const char* name) {
    if (vm.count(name) == 0) {
        return boost::none;
    }
    return vm[name].as<T>();
}

asio::ssl::context load_tls_client_ctx_from_file(const std::string& path, const char* for_whom);
asio::ssl::context load_tls_client_ctx_from_string(const std::string& ctx_str, const char* for_whom);

ClientConfig::ClientConfig(int argc, char* argv[])
{
    using namespace std;
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

    auto desc = description_full();

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        _is_help = true;
        return;
    }

    if (auto opt = as_optional<string>(vm, "repo")) {
        _repo_root = fs::path(*opt);

        if (!fs::exists(_repo_root)) {
            throw error("No such directory: ", _repo_root);
        }

        if (!fs::is_directory(_repo_root)) {
            throw error("The path is not a directory: ", _repo_root);
        }
    }
    else {
        throw error("The '--repo' option is missing");
    }

    // Load the file with saved configuration options, if it exists
    // (or remove it if requested).
    {
        po::options_description desc_save = description_saved();
        fs::path ouinet_save_path = _repo_root/_ouinet_conf_save_file;
        if (vm["drop-saved-opts"].as<bool>()) {
            sys::error_code ignored_ec;
            fs::remove(ouinet_save_path, ignored_ec);
        } else if (fs::is_regular_file(ouinet_save_path)) {
            ifstream ouinet_conf(ouinet_save_path.string());
            po::store(po::parse_config_file(ouinet_conf, desc_save), vm);
            po::notify(vm);
        }
    }

    {
        fs::path ouinet_conf_path = _repo_root/_ouinet_conf_file;
        if (fs::is_regular_file(ouinet_conf_path)) {
            ifstream ouinet_conf(ouinet_conf_path.string());
            po::store(po::parse_config_file(ouinet_conf, desc), vm);
            po::notify(vm);
        }
    }

    if (vm.count("log-level")) {
        auto level = boost::algorithm::to_upper_copy(vm["log-level"].as<string>());
        auto ll_o = log_level_from_string(level);
        if (!ll_o)
            throw error("Invalid log level: ", level);
        logger.set_threshold(*ll_o);
        LOG_INFO("Log level set to: ", level);
    }

    if (vm["enable-log-file"].as<bool>()) {
        _is_log_file_enabled(true);
    }

    if (auto opt = as_optional<vector<string>>(vm, "bt-bootstrap-extra")) {
        for (const auto& btbsx : *opt) {
            // Better processing will take place later on, just very basic checking here.
            auto btbs_addr = bittorrent::bootstrap::parse_address(btbsx);
            if (!btbs_addr)
                throw error("Invalid BitTorrent bootstrap server: ", btbsx);
            _bt_bootstrap_extras.insert(*btbs_addr);
        }
    }

    if (auto opt = as_optional<unsigned int>(vm, "open-file-limit")) {
        increase_open_file_limit(*opt);
    }

    if (auto opt = as_optional<int>(vm, "max-cached-age")) {
        _max_cached_age = boost::posix_time::seconds(*opt);
    }

    if (auto opt = as_optional<int>(vm, "max-simultaneous-announcements")) {
        _max_simultaneous_announcements = *opt;
    }

    assert(vm.count("listen-on-tcp") && "--listen-on-tcp should have a default value");
    {
        auto opt_local_ep = parse::endpoint<asio::ip::tcp>(vm["listen-on-tcp"].as<string>());
        if (!opt_local_ep) {
            throw error("Failed to parse '--listen-on-tcp' argument as TCP endpoint");
        }
        _local_ep = *opt_local_ep;
    }

    if (auto opt = as_optional<uint16_t>(vm, "udp-mux-port")) {
        _udp_mux_port = *opt;
    }

    if (auto opt = as_optional<string>(vm, "injector-ep")) {
        auto injector_ep_str = *opt;

        if (!injector_ep_str.empty()) {
            auto opt = parse_endpoint(injector_ep_str);

            if (!opt) {
                throw error("Failed to parse endpoint: ", injector_ep_str);
            }

            _injector_ep = *opt;
        }
    }

    assert(vm.count("front-end-ep") && "--front-end-ep should have a default value");
    {
        auto opt_fe_ep = parse::endpoint<asio::ip::tcp>(vm["front-end-ep"].as<string>());
        if (!opt_fe_ep) {
            throw error("Failed to parse '--front-end-ep' argument");
        }
        _front_end_endpoint = *opt_fe_ep;
    }

    if (auto opt = as_optional<string>(vm, "front-end-access-token")) {
        if (opt->empty()) {
            throw error("--front-end-access-token must not be an empty string");
        }
        _front_end_access_token = *opt;
    }

    if (auto opt = as_optional<bool>(vm, "disable-bridge-announcement")) {
        _disable_bridge_announcement = *opt;
    }

    if (auto opt = as_optional<string>(vm, "client-credentials")) {
        auto cred = *opt;

        if (!cred.empty() && cred.find(':') == string::npos) {
            throw error(
                "The '--client-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred);
        }

        _client_credentials = move(cred);
    }

    auto maybe_set_pk = [&] (const string& opt_name, auto& pk) {
        if (auto opt = as_optional<string>(vm, opt_name.c_str())) {
            string value = *opt;

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
                throw error("Failed to parse Ed25519 public key: ", value);
            }
        }
    };

    maybe_set_pk("cache-http-public-key", _cache_http_pubkey);

    if (auto opt = as_optional<string>(vm, "cache-type")) {
        auto type_str = *opt;

        if (type_str == "bep5-http") {
            // https://redmine.equalit.ie/issues/14920#note-1
            _cache_type = CacheType::Bep5Http;

            LOG_DEBUG("Using bep5-http cache");

            if (!_cache_http_pubkey) {
                throw error(
                    "'--cache-type=bep5-http' must be used with '--cache-http-public-key'");
            }

            if (_injector_ep && _injector_ep->type == Endpoint::Bep5Endpoint) {
                throw error("A BEP5 injector endpoint is derived implicitly"
                            " when using '--cache-type=bep5-http',"
                            " but it is already set to: ", *_injector_ep);
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
            throw error("Unknown '--cache-type' argument: ", type_str);
        }

    }

    if (auto opt = as_optional<string>(vm, "injector-credentials")) {
        auto cred = *opt;

        if (!cred.empty()
          && cred.find(':') == string::npos) {
            throw error(util::str(
                "The '--injector-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred));
        }

        if (!_injector_ep) {
            throw error(
                "The '--injector-credentials' argument must be used with "
                "'--injector-ep'");
        }

        _injector_credentials[*_injector_ep] = cred;
    }

    if (_cache_type == CacheType::None) {
        LOG_WARN("Not using d-cache");
    }

    if (is_cache_enabled() && _cache_type == CacheType::Bep5Http && !_cache_http_pubkey) {
        throw error("BEP5/HTTP cache selected but no injector HTTP public key specified");
    }

    if (auto opt = as_optional<string>(vm, "cache-static-root")) {
        _cache_static_content_path = *opt;
        if (!fs::is_directory(_cache_static_content_path))
            throw error("No such directory: ", _cache_static_content_path);
        if (!vm.count("cache-static-repo")) {
            _cache_static_path = _cache_static_content_path / default_static_cache_subdir;
            LOG_INFO("No static cache repository given, assuming: ", _cache_static_path);
        }
    }
    if (auto opt = as_optional<string>(vm, "cache-static-repo")) {
        _cache_static_path = *opt;
        if (!vm.count("cache-static-root"))
            throw error("'--cache-static-root' must be explicity given when using a static cache");
    }
    if (!_cache_static_path.empty() && !fs::is_directory(_cache_static_path))
        throw error("No such directory: ", _cache_static_path);

    if (auto opt = as_optional<string>(vm, "local-domain")) {
        auto local_domain = *opt;
        auto tld_rx = boost::regex("[-0-9a-zA-Z]+");
        if (!boost::regex_match(local_domain, tld_rx)) {
            throw error("Invalid TLD for '--local-domain': ", local_domain);
        }
        _local_domain = boost::algorithm::to_lower_copy(local_domain);
    }

    if (auto opt = as_optional<string>(vm, "origin-doh-base")) {
        auto doh_base = *opt;
        _origin_doh_endpoint = doh::endpoint_from_base(doh_base);
        if (!_origin_doh_endpoint)
            throw error(util::str(
                    "Invalid URL for '--origin-doh-base': ", doh_base));
    }

    _metrics = MetricsConfig::parse(vm);

    save_persistent();  // only if no errors happened
}

std::unique_ptr<MetricsConfig> MetricsConfig::parse(const boost::program_options::variables_map& vm) {
    bool enable_on_start = false;
    boost::optional<util::url_match> server_url;
    boost::optional<std::string> server_token;
    boost::optional<asio::ssl::context> server_cacert;
    std::optional<metrics::EncryptionKey> encryption_key;

    if (auto opt = as_optional<std::string>(vm, "metrics-server-url")) {
        util::url_match url_match;
        if (!util::match_http_url(*opt, url_match)) {
            throw error(
                    "The '--metrics-server-url' argument must be a valid URL");
        }
        server_url = std::move(url_match);
    }

    if (auto opt = as_optional<bool>(vm, "metrics-enable-on-start")) {
        if (*opt) {
            if (!server_url) {
                throw error("--metrics-enable-on-start must be used with --metrics-server-url");
            }
            enable_on_start = *opt;
        }
    }

    if (auto opt = as_optional<std::string>(vm, "metrics-server-token")) {
        if (!server_url) {
            throw error("The --metrics-server-token must be used with --metrics-server-url");
        }
        server_token = *opt;
    }


    auto server_cacert_str = as_optional<std::string>(vm, "metrics-server-cacert");
    auto server_cacert_file = as_optional<std::string>(vm, "metrics-server-cacert-file");

    if (server_cacert_str && server_cacert_file) {
        throw error("Only one of the --metrics-server-cacert and --metrics-server-cacert-file options may be specified");
    }

    if ((server_cacert_str || server_cacert_file) && !server_url) {
        throw error("--metrics-server-cacert and --metrics-server-cacert-file can only be used together with --metrics-server-url");
    }

    if (server_cacert_str) {
        server_cacert = load_tls_client_ctx_from_string(*server_cacert_str, "metrics server");
    } else if (server_cacert_file) {
        server_cacert = load_tls_client_ctx_from_file(*server_cacert_file, "metrics server");
    }

    if (server_url) {
        if (auto opt = as_optional<std::string>(vm, "metrics-encryption-key")) {
            encryption_key = metrics::EncryptionKey::validate(*opt);
            if (!encryption_key) {
                throw error("Failed to validate --metrics-encryption-key");
            }
        } else {
            throw error("--metrics-server-url must be used with --metrics-encryption-key");
        }
    }

    if (!server_url) return nullptr;

    return std::unique_ptr<MetricsConfig>(
            new MetricsConfig {
                enable_on_start,
                std::move(*server_url),
                std::move(server_token),
                std::move(server_cacert),
                std::move(*encryption_key)
            });
}

asio::ssl::context load_tls_client_ctx_from_string(const std::string& cert_str, const char* for_whom) {
        asio::ssl::context ctx{asio::ssl::context::tls_client};
        sys::error_code ec;

        ctx.add_certificate_authority(
                asio::const_buffer(cert_str.data(), cert_str.size()), ec);

        if (ec) {
            throw error("Failed to add tls certificate for ", for_whom, ":", ec.message(), "\n"
                       , "The certificate passed:\n"
                       , cert_str, "\n");
        }

        ctx.set_verify_mode(asio::ssl::verify_peer, ec);

        if (ec) {
            throw std::runtime_error(
                util::str("Failed to set verification mode for ", for_whom, " certificate:", ec.message()));
        }
        return ctx;
}

asio::ssl::context load_tls_client_ctx_from_file(const std::string& path, const char* for_whom) {
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    sys::error_code ec;

    ctx.load_verify_file(path, ec);

    if (ec) {
        throw error("Failed to read tls certificate for ", for_whom, " from \""
                   , path
                   , "\" error:", ec.message());
    }

    ctx.set_verify_mode(asio::ssl::verify_peer, ec);

    if (ec) {
        throw error("Failed to set verification mode for ", for_whom, " certificate:", ec.message());
    }
    return ctx;
}

} // namespace
