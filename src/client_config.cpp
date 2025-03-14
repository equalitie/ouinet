#include <boost/optional/optional_io.hpp>
#include "client_config.h"

namespace ouinet {

template<class... Args>
inline
std::runtime_error error(Args&&... args) {
    return std::runtime_error(util::str(std::forward<Args>(args)...));
}

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

    if (!vm.count("repo")) {
        throw error("The '--repo' option is missing");
    }

    _repo_root = fs::path(vm["repo"].as<string>());

    if (!fs::exists(_repo_root)) {
        throw error("No such directory: ", _repo_root);
    }

    if (!fs::is_directory(_repo_root)) {
        throw error("The path is not a directory: ", _repo_root);
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
        if (!fs::is_regular_file(ouinet_conf_path)) {
            throw error("The path ", _repo_root, " does not contain the "
                       , _ouinet_conf_file, " configuration file");
        }
        ifstream ouinet_conf(ouinet_conf_path.string());
        po::store(po::parse_config_file(ouinet_conf, desc), vm);
        po::notify(vm);
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

    if (vm.count("bt-bootstrap-extra")) {
        for (const auto& btbsx : vm["bt-bootstrap-extra"].as<vector<string>>()) {
            // Better processing will take place later on, just very basic checking here.
            auto btbs_addr = bittorrent::bootstrap::parse_address(btbsx);
            if (!btbs_addr)
                throw error("Invalid BitTorrent bootstrap server: ", btbsx);
            _bt_bootstrap_extras.insert(*btbs_addr);
        }
    }

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (vm.count("max-cached-age")) {
        _max_cached_age = boost::posix_time::seconds(vm["max-cached-age"].as<int>());
    }

    if (vm.count("max-simultaneous-announcements")) {
        _max_simultaneous_announcements = vm["max-simultaneous-announcements"].as<int>();
    }

    assert(vm.count("listen-on-tcp"));
    {
        auto opt_local_ep = parse::endpoint<asio::ip::tcp>(vm["listen-on-tcp"].as<string>());
        if (!opt_local_ep) {
            throw error("Failed to parse '--listen-on-tcp' argument");
        }
        _local_ep = *opt_local_ep;
    }

    if (vm.count("udp-mux-port")) {
        _udp_mux_port = vm["udp-mux-port"].as<uint16_t>();
    }

    if (vm.count("injector-ep")) {
        auto injector_ep_str = vm["injector-ep"].as<string>();

        if (!injector_ep_str.empty()) {
            auto opt = parse_endpoint(injector_ep_str);

            if (!opt) {
                throw error("Failed to parse endpoint: ", injector_ep_str);
            }

            _injector_ep = *opt;
        }
    }

    assert(vm.count("front-end-ep"));
    {
        auto opt_fe_ep = parse::endpoint<asio::ip::tcp>(vm["front-end-ep"].as<string>());
        if (!opt_fe_ep) {
            throw error("Failed to parse '--front-end-ep' argument");
        }
        _front_end_endpoint = *opt_fe_ep;
    }

    if (vm.count("disable-bridge-announcement")) {
        _disable_bridge_announcement = vm["disable-bridge-announcement"].as<bool>();
    }

    if (vm.count("client-credentials")) {
        auto cred = vm["client-credentials"].as<string>();

        if (!cred.empty() && cred.find(':') == string::npos) {
            throw error(
                "The '--client-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred);
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
                throw error("Failed to parse Ed25519 public key: ", value);
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

    if (vm.count("injector-credentials")) {
        auto cred = vm["injector-credentials"].as<string>();

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

    if (vm.count("cache-static-root")) {
        _cache_static_content_path = vm["cache-static-root"].as<string>();
        if (!fs::is_directory(_cache_static_content_path))
            throw error("No such directory: ", _cache_static_content_path);
        if (!vm.count("cache-static-repo")) {
            _cache_static_path = _cache_static_content_path / default_static_cache_subdir;
            LOG_INFO("No static cache repository given, assuming: ", _cache_static_path);
        }
    }
    if (vm.count("cache-static-repo")) {
        _cache_static_path = vm["cache-static-repo"].as<string>();
        if (!vm.count("cache-static-root"))
            throw error("'--cache-static-root' must be explicity given when using a static cache");
    }
    if (!_cache_static_path.empty() && !fs::is_directory(_cache_static_path))
        throw error("No such directory: ", _cache_static_path);

    if (vm.count("local-domain")) {
        auto tld_rx = boost::regex("[-0-9a-zA-Z]+");
        auto local_domain = vm["local-domain"].as<string>();
        if (!boost::regex_match(local_domain, tld_rx)) {
            throw error("Invalid TLD for '--local-domain': ", local_domain);
        }
        _local_domain = boost::algorithm::to_lower_copy(local_domain);
    }

    if (vm.count("origin-doh-base")) {
        auto doh_base = vm["origin-doh-base"].as<string>();
        _origin_doh_endpoint = doh::endpoint_from_base(doh_base);
        if (!_origin_doh_endpoint)
            throw error(util::str(
                    "Invalid URL for '--origin-doh-base': ", doh_base));
    }

    if (auto opt = as_optional<string>(vm, "metrics-server-url")) {
        util::url_match url_match;
        if (!util::match_http_url(*opt, url_match)) {
            throw error(
                    "The '--metrics-server-url' argument must be a valid URL");
        }
        _metrics_server_url = url_match;
    }

    if (vm.count("metrics-enable-on-start")) {
        _metrics_enable_on_start = vm["metrics-enable-on-start"].as<bool>();
        if (_metrics_enable_on_start && !_metrics_server_url) {
            throw error("--metrics-enable-on-start must be used with --metrics-server-url");
        }
    }

    if (vm.count("metrics-server-token")) {
        if (!_metrics_server_url) {
            throw error("The '--metrics-server-token' must be used with '--metrics-server'");
        }
        _metrics_server_token = vm["metrics-server-token"].as<string>();
    }


    auto metrics_server_tls_cert = as_optional<string>(vm, "metrics-server-tls-cert");
    auto metrics_server_tls_cert_file = as_optional<string>(vm, "metrics-server-tls-cert-file");

    if (metrics_server_tls_cert && metrics_server_tls_cert_file) {
        throw error("Only one of the --metrics-server-tls-cert and --metrics-server-tls-cert-file options may be specified");
    } else if (metrics_server_tls_cert) {
        asio::ssl::context ctx{asio::ssl::context::tls_client};
        sys::error_code ec;
        ctx.add_certificate_authority(
                asio::const_buffer(
                    metrics_server_tls_cert->data(),
                    metrics_server_tls_cert->size()),
                ec);
        if (ec) {
            throw error("Failed to add tls certificate for metrics server:", ec.message());
        }
        ctx.set_verify_mode(asio::ssl::verify_peer, ec);
        if (ec) {
            throw std::runtime_error(
                util::str("Failed to set verification mode for metrics server certificate:", ec.message()));
        }
        _metrics_server_tls_cert = std::move(ctx);
    } else if (metrics_server_tls_cert_file) {
        asio::ssl::context ctx{asio::ssl::context::tls_client};
        sys::error_code ec;
        ctx.load_verify_file(*metrics_server_tls_cert_file, ec);
        if (ec) {
            throw error("Failed to read tls certificate for metrics server from \""
                       , metrics_server_tls_cert_file
                       , "\" error:", ec.message());
        }
        ctx.set_verify_mode(asio::ssl::verify_peer, ec);
        if (ec) {
            throw error("Failed to set verification mode for metrics server certificate:", ec.message());
        }
        _metrics_server_tls_cert = std::move(ctx);
    }

    save_persistent();  // only if no errors happened
}

} // namespace
