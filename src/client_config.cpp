#include <boost/optional/optional_io.hpp>
#include <boost/program_options.hpp>
#include "client_config.h"
#include "ssl/util.h"
#include "logger.h"
#include "parse/endpoint.h"
#include "bep5_swarms.h"
#include "util/bytes.h"
#include "util/debug.h"
#include "util/overloaded.h"
#include "util/str.h"
#include "config/util.h"
#include "config/i2p_service.h"
#ifndef __WIN32
#include "increase_open_file_limit.h"
#endif

namespace ouinet {

namespace po = boost::program_options;

#define _LOG_FILE_NAME "log.txt"
static const fs::path log_file_name{_LOG_FILE_NAME};

#define _DEFAULT_STATIC_CACHE_SUBDIR ".ouinet"
static const fs::path default_static_cache_subdir{_DEFAULT_STATIC_CACHE_SUBDIR};

bool ClientConfig::EnabledCaches::get(CacheType type) const {
    return std::visit(overloaded {
                [&] (const CacheType::Bep5Http&) { return Bep5Http; },
                [&] (const CacheType::Bep3HTTPOverI2P&) { return Bep3HTTPOverI2P; },
                [&] (const CacheType::Ouisync&) { return Ouisync; },
            },
            type.value);
}

void ClientConfig::EnabledCaches::set(CacheType type, bool value) {
    return std::visit(overloaded {
                [&] (const CacheType::Bep5Http&) { Bep5Http = value; },
                [&] (const CacheType::Bep3HTTPOverI2P&) { Bep3HTTPOverI2P = value; },
                [&] (const CacheType::Ouisync&) { Ouisync = value; },
            },
            type.value);
}

bool ClientConfig::EnabledCaches::is_any_cache_enabled() const {
    return Bep5Http || Bep3HTTPOverI2P || Ouisync;
}

bool ClientConfig::EnabledCaches::is_injecting_cache_enabled() const {
    return Bep5Http || Bep3HTTPOverI2P;
}

asio::ssl::context load_tls_client_ctx_from_file(const std::string& path, const std::string& for_whom);
asio::ssl::context load_tls_client_ctx_from_string(const std::string& ctx_str, const std::string& for_whom);

boost::program_options::options_description ClientConfig::description_full()
{
    using namespace std;

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
       ("bt-bootstrap-no-default", po::bool_switch()->default_value(false)
        , "Don't use default BitTorrent bootstrap servers."
          "When this option is enabled, only the servers specified with --bt-bootstrap-extra are used."
          "This option is persistent.")
       ("bt-allow-martians", po::bool_switch()->default_value(false)
        , "Allow BitTorrent DHT peers with invalid or suspicious endpoints. Useful mostly for testing.")
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
        , "HTTP proxy endpoint (in <IP>:<PORT> format). Set port to 0 for random port assigned by OS.")
       ("udp-mux-port"
       , po::value<uint16_t>()
       , "Port used by the UDP multiplexer in BEP5 and uTP interactions.")
       ("udp-mux-rx-limit"
       , po::value<uint32_t>()
       , "Max rate limit that's allowed for incoming packets to the "
         "UDP multiplexer. The value is expressed in Kbps. To leave it "
         "unlimited, set it to zero.")
       ("client-credentials", po::value<string>()
        , "<username>:<password> authentication pair for the client")
       ("tls-ca-cert-store-dir", po::value<string>(&_tls_ca_cert_store_dir)
        , "Path to the CA certificate store directory")
       ("tls-ca-cert-store-file", po::value<vector<string>>(&_tls_ca_cert_store_files)
        , "Add CA certificate store file")
       ("front-end-ep"
        , po::value<string>()->default_value("127.0.0.1:8078")
        , "Front-end's endpoint (in <IP>:<PORT> format). Set port to 0 for random port assigned by OS.")
        ("front-end-unix-socket-ep"
        , po::value<string>()
        , "Path to the front-end Unix socket. Absolute or relative to repo root.")
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

    add_i2p_service_options(services);

    po::options_description injector("Injector options");
    injector.add_options()
       ("injector-ep"
        , po::value<string>()
        , "Injector's endpoint as <TYPE>:<EP>, "
          "where <TYPE> can be \"tcp\", \"utp\",  "
          "and <EP> depends on the type of endpoint: "
          "<IP>:<PORT> for TCP and uTP"
          ", <IP>:<PORT>[,<OPTION>=<VALUE>...] for OBFS and Lampshade, "
          "<B32_PUBKEY>.b32.i2p or <B64_PUBKEY> for I2P"
       )
       ("injector-credentials", po::value<string>()
        , "<username>:<password> authentication pair for the injector")
       ("injector-tls-cert-file", po::value<string>(&_tls_injector_cert_path)
        , "Path to the injector's TLS certificate; enable TLS for TCP and uTP")
      ("i2p-hops-per-tunnel", po::value<size_t>()
        , "number intermediary hops to be used for I2P garlic routing.")
      ("i2p-bep3-tracker", po::value<string>()
        , "I2P address of the BEP3 tracker "
          "(<B32_PUBKEY>.b32.i2p or <B64_PUBKEY>)")
       ;

    po::options_description cache("Cache options");
    cache.add_options()
       ("cache-type", po::value<vector<string>>()
        , "Type of d-cache {bep5-http, bep3-http-over-i2p, ouisync}. "
          "Can be used multiple times to enable different types of cache types")
       ("cache-http-public-key"
        , po::value<string>()
        , "Public key for HTTP signatures in the BEP5/HTTP cache "
          "(hex-encoded or Base32-encoded)")
       ("max-cached-age"
        , po::value<decltype(_max_cached_age.total_seconds())>()->default_value(_max_cached_age.total_seconds())
        , "Discard cached content older than this many seconds "
          "(0: discard all; -1: discard none)")
       ("max-simultaneous-announcements"
        , po::value<decltype(_max_simultaneous_announcements)>()->default_value(_max_simultaneous_announcements)
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
       ("disable-proxy-access", po::bool_switch(&_disable_proxy_access)->default_value(false)
        , "Disable proxied access to the origin (via the injector). "
          "This option is persistent.")
       ("local-domain"
        , po::value<string>()->default_value("local")
        , "Always use origin access and never use cache for this TLD")
        ("allow-private-targets", po::bool_switch(&_allow_private_targets)->default_value(false)
        , "Allows using non-origin channels, like injectors, dist-cache, etc, "
          "to fetch targets using private addresses. "
          "Example: 192.168.1.13, 10.8.0.2, 172.16.10.8, etc.")
        ;

    po::options_description dns("DNS options");
    dns.add_options()
       ("dns-protocol", po::value<vector<string>>()
                            ->composing()
                            ->default_value(dns_default_protocols,
                                            util::join(dns_default_protocols, ","))
        ,"DNS protocols used by the resolver. This option can be set to: plain or https. "
          "When plain is selected, the resolver will establish UDP/TCP unencrypted connections with "
          "the nameservers. The option can be used multiple times to select more than one protocol.")
       ;

    po::options_description metrics("Metrics options");
    metrics.add_options()
       ("metrics-enable-on-start", po::bool_switch()->default_value(false)
        , "Enable metrics at startup. Must be used with --metrics-server-url")
       ("metrics-server-url", po::value<std::vector<string>>()->composing()
        , "URL to the metrics server where statistics/metrics records will be sent over HTTP. "
          "This option can be repeated to specify multiple servers.")
       ("metrics-server-token", po::value<std::vector<string>>()->composing()
        , "Token sent to the server as 'token: <TOKEN>' HTTP header. "
          "This option must be specified once for each server specified with --metrics-server-url. "
          "To use no token for a particular server, set this option to '.'. "
          "To use no token for any server, this option can be omitted.")
       ("metrics-server-cacert", po::value<std::vector<string>>()->composing()
        , "TLS CA certificate for the metrics server. "
          "This option must be specified once for each server specified with --metrics-server-url. "
          "To use default certificate for a particular server, set this option to '.'. "
          "To read the certificate from a file, set this option to the path to the file prefixed with '@' "
          "(example: '@/path/to/cert'). "
          "To use default certificates for all servers, this option can be omitted.")
       ("metrics-encryption-key", po::value<string>()
        , "Key to encrypt metrics records with. To generate the (public) encryption key, you can use "
          "the following. \n"
          "   First generate the private key:\n"
          "     `openssl genpkey -algorithm x25519 -out private_key.pem`\n"
          "   Then get the public encryption key:\n"
          "     `openssl pkey -in private_key.pem -pubout -out public_key.pem`"
          )
       ("metrics-delete-after"
        , po::value<uint64_t>()->default_value(metrics::default_delete_after_seconds)
        , "Metrics records older than this duration will be deleted. "
          "The value is expressed in seconds.")
       ;

    po::options_description desc;
    desc
        .add(general)
        .add(services)
        .add(injector)
        .add(cache)
        .add(requests)
        .add(dns)
        .add(metrics);
    return desc;
}

// A restricted version of the above, only accepting persistent configuration options,
// with no defaults nor descriptions.
boost::program_options::options_description ClientConfig::description_saved()
{
    po::options_description desc;
    desc.add_options()
        ("log-level", po::value<std::string>())
        ("enable-log-file", po::bool_switch())
        ("bt-bootstrap-extra", po::value<std::vector<std::string>>()->composing())
        ("bt-bootstrap-no-default", po::bool_switch(&_bt_bootstrap_no_default))
        ("disable-origin-access", po::bool_switch(&_disable_origin_access))
        ("disable-injector-access", po::bool_switch(&_disable_injector_access))
        ("disable-proxy-access", po::bool_switch(&_disable_proxy_access))
        ;
    return desc;
}

void ClientConfig::save_persistent() {
    using namespace std;
    ostringstream ss;

    ss << "log-level = " << log_level() << endl;
    ss << "enable-log-file = " << is_log_file_enabled() << endl;

    for (const auto& btbs_addr : _bt_bootstrap_extras) {
        ss << "bt-bootstrap-extra = " << btbs_addr << endl;
    }
    ss << "bt-bootstrap-no-default = " << _bt_bootstrap_no_default << endl;

    ss << "disable-origin-access = " << _disable_origin_access << endl;
    ss << "disable-injector-access = " << _disable_injector_access << endl;
    ss << "disable-proxy-access = " << _disable_proxy_access << endl;

    try {
        fs::path ouinet_save_path = _repo_root/_ouinet_conf_save_file;
        LOG_DEBUG("Saving persistent options");
        ofstream(ouinet_save_path.string(), fstream::out | fstream::trunc) << ss.str();
    } catch (const exception& e) {
        LOG_ERROR("Failed to save persistent options: ", e.what());
    }
}

ClientConfig::ClientConfig(int argc, const char* argv[])
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
        get_logger().set_threshold(*ll_o);
        LOG_INFO("Log level set to: ", level);
    }

    if (vm["enable-log-file"].as<bool>()) {
        _is_log_file_enabled(true);
    }

    for (const auto& btbsx : as_vector<string>(vm, "bt-bootstrap-extra")) {
        // Better processing will take place later on, just very basic checking here.
        auto btbs_addr = bittorrent::bootstrap::parse_address(btbsx);
        if (!btbs_addr)
            throw error("Invalid BitTorrent bootstrap server: ", btbsx);
        _bt_bootstrap_extras.insert(*btbs_addr);
    }

    if (vm["bt-bootstrap-no-default"].as<bool>()) {
        _bt_bootstrap_no_default = true;
    }

    if (vm["bt-allow-martians"].as<bool>()) {
        _bt_allow_martians = true;
    }

#ifndef __WIN32
    if (auto opt = as_optional<unsigned int>(vm, "open-file-limit")) {
        increase_open_file_limit(*opt);
    }
#endif

    if (auto opt = as_optional<decltype(_max_cached_age.total_seconds())>(vm, "max-cached-age")) {
        _max_cached_age = boost::posix_time::seconds(*opt);
    }

    if (auto opt = as_optional<decltype(_max_simultaneous_announcements)>(vm, "max-simultaneous-announcements")) {
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

    if (auto opt = as_optional<uint32_t>(vm, "udp-mux-rx-limit")) {
        _udp_mux_rx_limit = *opt;
    }

    if (auto opt = as_optional<string>(vm, "injector-ep")) {
        auto injector_ep_str = *opt;

        if (!injector_ep_str.empty()) {
            auto opt = Endpoint::parse(injector_ep_str);

            if (!opt) {
                throw error("Failed to parse endpoint: ", injector_ep_str);
            }

            std::move(*opt).visit( overloaded {
                [&] (asio::ip::tcp::endpoint ep) {
                    _injector_endpoints.get<CacheType::Bep5Http>() = std::move(ep);
                },
                [&] (Endpoint::Utp ep) {
                    _injector_endpoints.get<CacheType::Bep5Http>() = std::move(ep);
                },
                [&] (Endpoint::Bep5) {
                    throw error("Bep5 endpoints are set implicitly");
                },
                [&] (I2pAddress ep) {
                    _injector_endpoints.get<CacheType::Bep3HTTPOverI2P>() = std::move(ep);
                }
            });
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

    if (auto opt = as_optional<string>(vm, "front-end-unix-socket-ep")) {
        if (opt->empty()) {
            throw error("--front-end-unix-socket-ep must not be an empty string");
        }
        fs::path socket_path(*opt);
        if (!socket_path.is_absolute()) {
            socket_path = _repo_root / socket_path;
        }
        _front_end_unix_socket_endpoint = socket_path.generic_string();
    }

    if (auto opt = as_optional<string>(vm, "front-end-access-token")) {
        if (opt->empty()) {
            throw error("--front-end-access-token must not be an empty string");
        }
        _front_end_access_token = *opt;
    }

    if (auto opt = as_optional<string>(vm, "proxy-access-token")) {
        if (opt->empty()) {
            throw error("--proxy-access-token must not be an empty string");
        }
        _proxy_access_token = *opt;
    }

    if (auto opt = as_optional<bool>(vm, "disable-bridge-announcement")) {
        _disable_bridge_announcement = *opt;
    }

    if (auto opt = as_optional<uint64_t>(vm, "request-body-limit")) {
        _max_req_body_size = *opt;
    }

    if (vm.count("add-request-field")) {
        auto fields = vm["add-request-field"].as<std::vector<std::string>>();
        for (auto field : fields) {
            auto pos = field.find(':');
            if (pos != string::npos) {
                _add_request_fields[field.substr(0, pos)] = field.substr(pos + 1);
            } else {
                _add_request_fields[field] = "";
            }
        }
    }

    if (auto opt = as_optional<string>(vm, "client-credentials")) {
        auto cred = *opt;

        if (!cred.empty() && cred.find(':') == string::npos) {
            throw error(
                "The '--client-credentials' argument expects a string "
                "in the format <username>:<password>, but the provided "
                "string is missing a colon: ", cred);
        }

        _client_credentials = std::move(cred);
    }

    auto maybe_set_pk = [&] (const string& opt_name, auto& pk) {
        if (auto opt = as_optional<string>(vm, opt_name.c_str())) {
            string value = *opt;

            pk = sign::PublicKey::from_hex(value);

            if (!pk) {  // attempt decoding from Base32
                auto pk_s = util::base32_decode(value);
                if (pk_s.size() == sign::PublicKey::size) {
                    auto pk_a = util::bytes::to_array<uint8_t, sign::PublicKey::size>(pk_s);
                    pk = sign::PublicKey(std::move(pk_a));
                }
            }

            if (!pk) {
                throw error("Failed to parse Ed25519 public key: ", value);
            }
        }
    };

    maybe_set_pk("cache-http-public-key", _cache_http_pubkey);

    for (auto type_str : as_vector<string>(vm, "cache-type")) {
        auto cache_type = CacheType::from_cmd_arg(type_str);

        if (!cache_type) {
            throw error("Unknown '--cache-type' argument: \"", type_str, "\"");
        }

        std::visit(overloaded {
                [&] (const CacheType::Bep5Http& type) {
                    LOG_INFO("Enabling bep5-http cache");

                    // https://redmine.equalit.ie/issues/14920#note-1
                    _enabled_caches.set(type, true);

                    if (!_cache_http_pubkey) {
                        throw error(
                            "'--cache-type=bep5-http' must be used with '--cache-http-public-key'");
                    }

                    auto& injector_ep = _injector_endpoints.get<CacheType::Bep5Http>();

                    if (!injector_ep) {
                        injector_ep = Endpoint::Bep5{
                            bep5::compute_injector_swarm_name(*_cache_http_pubkey, http_::protocol_version_current)
                        };
                    }
                },
                [&] (const CacheType::Bep3HTTPOverI2P& type) {
                    LOG_INFO("Enabling bep3-http cache over i2p");

                    _enabled_caches.set(type, true);

                    if (!_cache_http_pubkey) {
                        throw std::runtime_error(
                            "'--cache-type=bep3-http-over-i2p' must be used with '--cache-http-public-key'");
                    }
                },
                [&] (const CacheType::Ouisync& type) {
                    LOG_INFO("Enabling Ouisync cache");

                    _enabled_caches.set(type, true);

                    if (auto token_opt = as_optional<string>(vm, "ouisync-page-index")) {
                        _ouisync = OuisyncCacheConfig { *token_opt };
                    } else {
                        throw error("Argument --cache-type=ouisync requires --ouisync-page-index=<page_index_repo_read_token>");
                    }
                },
            },
            cache_type->value);
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

        if (!_injector_endpoints.get<CacheType::Bep5Http>() && !_injector_endpoints.get<CacheType::Bep3HTTPOverI2P>()) {
            throw error(
                "The '--injector-credentials' argument must be used with "
                "'--injector-ep'");
        }

        _injector_credentials = cred;
    }

    if (vm.count("i2p-hops-per-tunnel")) {
        auto no_of_hops_per_tunnel = vm["i2p-hops-per-tunnel"].as<size_t>();

        if (!no_of_hops_per_tunnel or no_of_hops_per_tunnel > _MAX_I2P_HOPS) {
            throw std::runtime_error(util::str(
                "The '--i2p-hops-per-tunnel' argument expects an integer "
                "between 1 and 8"
                ));
        }

        if (!_injector_endpoints.get<CacheType::Bep3HTTPOverI2P>() && !_enabled_caches.get(CacheType::Bep3HTTPOverI2P{})) {
            throw std::runtime_error(
                "The '--i2p-hops-per-tunnel' argument must be used with "
                "'--injector-ep' with an i2p injector or with "
                "--cache-type=bep3-http-over-i2p ");
        }

        _i2p_hops_per_tunnel = no_of_hops_per_tunnel;
    }

    if (auto opt = as_optional<string>(vm, "i2p-bep3-tracker")) {
        if (!_enabled_caches.get(CacheType::Bep3HTTPOverI2P{})) {
            throw std::runtime_error(
                "'--i2p-bep3-tracker' can only be used with '--cache-type=bep3-http-over-i2p'");
        }
        auto addr = I2pAddress::parse(*opt);
        if (!addr) {
            throw std::runtime_error(
                "invalid i2p address of bep3 tracker was provided");
        }
        _i2p_bep3_tracker = std::move(*addr);
    }

    if (_enabled_caches.get(CacheType::Bep3HTTPOverI2P{}) && !_i2p_bep3_tracker) {
        throw std::runtime_error(
            "'--cache-type=bep3-http-over-i2p' requires '--i2p-bep3-tracker' to be set");
    }

    if (!_enabled_caches.is_any_cache_enabled()) {
        LOG_WARN("Not using d-cache");
    }

    if (_enabled_caches.get(CacheType::Bep5Http{}) && !_cache_http_pubkey) {
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

    for (const auto& proto_name : as_vector<string>(vm, "dns-protocol")) {
        dns::bridge::Protocol proto;
        try {
            proto = dns::bridge::str_to_proto(proto_name);
        } catch (const rust::Error&) {
            throw error("Invalid argument for option --dns-protocol: ", proto_name);
        }

        if ( ranges::find(_dns_config.protocols, proto) ==  _dns_config.protocols.end())
            _dns_config.protocols.emplace_back(proto);
    }

    LOG_DEBUG( "DNS protocols enabled: ["
             , dns::Resolver::protos_to_str(_dns_config.protocols)
             , "]");

    if (vm["allow-private-targets"].as<bool>()) {
        _allow_private_targets = true;
    }

    {
        _origin_ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
        ssl::util::load_tls_ca_certificates(_origin_ssl_ctx, _tls_ca_cert_store_dir);
        for (auto& verify_file : _tls_ca_cert_store_files) {
            sys::error_code ec;
            _origin_ssl_ctx.load_verify_file(verify_file, ec);
            if (ec) throw error("Failed to load origin CA certificate from \"", verify_file, "\"");
        }
    }

    _metrics = MetricsConfig::parse(vm);

    _i2p_service_config = parse_i2p_service_config(vm, _repo_root);

    save_persistent();  // only if no errors happened
}

std::unique_ptr<MetricsConfig> MetricsConfig::parse(const boost::program_options::variables_map& vm) {
    auto raw_server_urls = as_vector<std::string>(vm, "metrics-server-url");
    auto raw_server_tokens = as_vector<std::string>(vm, "metrics-server-token");
    auto raw_server_cacerts = as_vector<std::string>(vm, "metrics-server-cacert");

    if (!raw_server_urls.empty()) {
        if (!raw_server_tokens.empty() && raw_server_tokens.size() != raw_server_urls.size()) {
            throw error(
                "Wrong number of occurrences of --metrics-server-token (",
                raw_server_tokens.size(),
                ") - it must be the same as the number of occurrences of --metrics-server-url (",
                raw_server_urls.size(), ") or zero"
            );
        }

        if (!raw_server_cacerts.empty() && raw_server_cacerts.size() != raw_server_urls.size()) {
            throw error(
                "Wrong number of occurrences of --metrics-server-cacert (",
                raw_server_cacerts.size(),
                ") - it must be the same as the number of occurrences of --metrics-server-url (",
                raw_server_urls.size(), ") or zero"
            );
        }
    } else {
        if (!raw_server_tokens.empty()) {
            throw error("--metrics-server-token can only be used together with --metrics-server-url");
        }

        if (!raw_server_cacerts.empty()) {
            throw error("--metrics-server-cacert can only be used together with --metrics-server-url");
        }
    }

    std::vector<MetricsServerConfig> servers;
    for (size_t i = 0; i < raw_server_urls.size(); ++i) {
        auto raw_url = std::move(raw_server_urls[i]);
        auto raw_token = i < raw_server_tokens.size() ? std::move(raw_server_tokens[i]) : "";
        auto raw_cacert = i < raw_server_cacerts.size() ? std::move(raw_server_cacerts[i]) : "";

        auto url = util::Url::from(raw_url);
        if (!url) {
            throw error(
                "Invalid argument in the ",
                util::ordinal(i + 1),
                " occurrence of --metrics-server-url: ",
                std::quoted(raw_url),
                " is not a valid URL"
            );
        }

        std::optional<std::string> token;
        if (!raw_token.empty() && raw_token != ".") {
            token = std::move(raw_token);
        }

        std::optional<asio::ssl::context> cacert;
        if (!raw_cacert.empty() && raw_cacert != ".") {
            auto name = util::str("metrics server ", url->reassemble());

            if (raw_cacert.starts_with("@")) {
                cacert = load_tls_client_ctx_from_string(raw_cacert.substr(1), name);
            } else {
                cacert = load_tls_client_ctx_from_file(raw_cacert, name);
            }
        }

        servers.emplace_back(
            std::move(*url),
            std::move(token),
            std::move(cacert)
        );
    }

    bool enable_on_start = false;
    if (auto opt = as_optional<bool>(vm, "metrics-enable-on-start")) {
        if (*opt) {
            if (servers.empty()) {
                throw error("--metrics-enable-on-start must be used with --metrics-server-url");
            }
            enable_on_start = *opt;
        }
    }

    std::optional<metrics::EncryptionKey> encryption_key;
    if (!servers.empty()) {
        if (auto opt = as_optional<std::string>(vm, "metrics-encryption-key")) {
            auto key_str = *opt;
            if ( !key_str.starts_with("-----BEGIN") ) {
                key_str = "-----BEGIN PUBLIC KEY-----\n" + key_str + "\n"
                          "-----END PUBLIC KEY-----\n";
            }
            encryption_key = metrics::EncryptionKey::validate(key_str);
            if (!encryption_key) {
                throw error("Failed to validate --metrics-encryption-key");
            }
        } else {
            throw error("--metrics-server-url must be used with --metrics-encryption-key");
        }
    }

    if (servers.empty()) {
        return nullptr;
    }

    auto delete_after_seconds = *as_optional<std::uint64_t>(vm, "metrics-delete-after");

    return std::make_unique<MetricsConfig>(
        enable_on_start,
        std::move(servers),
        std::move(*encryption_key),
        delete_after_seconds
    );
}

asio::ssl::context load_tls_client_ctx_from_string(const std::string& cert_str, const std::string& for_whom) {
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

asio::ssl::context load_tls_client_ctx_from_file(const std::string& path, const std::string& for_whom) {
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

boost::optional<std::string> ClientConfig::bep5_bridge_swarm_name() const {
    if (!_cache_http_pubkey) return boost::none;
    return bep5::compute_bridge_swarm_name( *_cache_http_pubkey
                                          , http_::protocol_version_current);
}

void ClientConfig::_is_log_file_enabled(bool v) {
    if (!v) {
        get_logger().log_to_file("");
        return;
    }

    if (_is_log_file_enabled()) return;

    auto current_log_path = get_logger().current_log_file();
    auto ouinet_log_path = current_log_path.empty()
        ? (_repo_root / log_file_name).string()
        : current_log_path;

    get_logger().log_to_file(ouinet_log_path);
    LOG_INFO("Log file set to: ", ouinet_log_path);
}

void ClientConfig::describe(std::ostream& os) {
    os << description_full();
}

} // namespace
