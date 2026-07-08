#pragma once

#include <set>
#include <sstream>
#include <vector>

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/filesystem.hpp>

#include "namespaces.h"
#include "cache_control.h"
#include "util.h"
#include "api.h"
#include "util/sign.h"
#include "util/overloaded.h"
#include "endpoint.h"
#include "constants.h"
#include "bittorrent/bootstrap.h"
#include "cxx/dns.h"
#include "ouiservice/i2p/address.h"
#include "logger.h"
#include "cache_type.h"

namespace boost::program_options {
    class variables_map;
    class options_description;
} // namespace

namespace ouinet {

//TODO: move this to somewhere where both client and injector config has access to
#define _MAX_I2P_HOPS 8

// Announcements are processed one at a time in Android to avoid increasing battery usage
#ifdef __ANDROID__
    const size_t default_max_simultaneous_announcements = 1;
#else
    const size_t default_max_simultaneous_announcements = 16;
#endif

struct MetricsConfig {
    bool enable_on_start = false;
    util::Url server_url;
    boost::optional<std::string> server_token;
    boost::optional<asio::ssl::context> server_cacert;
    metrics::EncryptionKey encryption_key;
    uint64_t delete_after_seconds;

    static std::unique_ptr<MetricsConfig> parse(const boost::program_options::variables_map&);
};

struct OuisyncClientConfig {
    // Read token for the page index repository which contains directories one per host name
    // and inside them crawls of corresponding websites.
    // If empty, using ouisync as cache is disabled.
    std::string page_index_token;
    // Whether to use Ouisync as network transport layer.
    bool transport = false;
    // Endpoints to bind Ouisync networking to. Should typically contain one IPv4 and one IPv6
    // endpoint.
    std::vector<boost::asio::ip::udp::endpoint> udp_endpoints = {
        { boost::asio::ip::address_v4::any(), 0 },
        { boost::asio::ip::address_v6::any(), 0 }
    };
};

// ----

template<class... TypePairs> struct TypeValueMap {};

template<class K, class V, class ... RestPairs>
struct TypeValueMap<std::pair<K, V>, RestPairs...> : public TypeValueMap<RestPairs...> {
    std::pair<K, V> value;

    template<class T> auto& get() {
        if constexpr (std::is_same_v<K, T>) {
            return value.second;
        } else {
            return TypeValueMap<RestPairs...>::template get<T>();
        }
    }

    template<class T> const auto& get() const {
        if constexpr (std::is_same_v<K, T>) {
            return value.second;
        } else {
            return TypeValueMap<RestPairs...>::template get<T>();
        }
    }
};

// ----

class OUINET_CLIENT_API ClientConfig {
    struct EnabledCaches {
        bool Bep5Http = false;
        bool Bep3HTTPOverI2P = false;
        bool Ouisync = false;

        bool get(CacheType) const;
        void set(CacheType, bool);
        bool is_any_cache_enabled() const;
        bool is_injecting_cache_enabled() const;
    };

    using InjectorEndpoints = TypeValueMap<
        std::pair<CacheType::Bep5Http, std::optional<Endpoint>>,
        // TODO: Also allow swarms
        std::pair<CacheType::Bep3HTTPOverI2P, std::optional<I2pAddress>>
    >;

public:
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

    template<class CacheType>
    const auto& injector_endpoint() const {
        return _injector_endpoints.get<CacheType>();
    }

    const std::string& tls_injector_cert_path() const {
        return _tls_injector_cert_path;
    }

    asio::ssl::context& origin_ssl_ctx() {
        return _origin_ssl_ctx;
    }

    size_t i2p_hops_per_tunnel() const {
      return _i2p_hops_per_tunnel;
    }

    const boost::optional<I2pAddress>& i2p_bep3_tracker() const {
      return _i2p_bep3_tracker;
    }

    const asio::ip::tcp::endpoint& local_endpoint() const {
        return _local_ep;
    }

    std::optional<uint16_t> udp_mux_port() const {
        return _udp_mux_port;
    }

    uint32_t udp_mux_rx_limit() const {
        // Value in Kbps
        return _udp_mux_rx_limit;
    }

    uint32_t udp_mux_rx_limit_in_bytes() const {
        // The value is set in Kbps in the configuration but required in bytes
        // by `UdpMultiplexer::maintain_max_rate_bytes_per_sec`.
        return _udp_mux_rx_limit * 1000 / 8;
    }

    bool is_cache_enabled(CacheType type) const { return _enabled_caches.get(type); }
    bool is_injecting_cache_enabled() const { return _enabled_caches.is_injecting_cache_enabled(); }

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

    boost::optional<std::string> bep5_bridge_swarm_name() const;

    bool is_bridge_announcement_enabled() const {
        return !_disable_bridge_announcement;
    }

    boost::optional<std::string>
    injector_credentials() const {
        return _injector_credentials;
    }

    asio::ip::tcp::endpoint front_end_endpoint() const {
        return _front_end_endpoint;
    }

    asio::local::stream_protocol::endpoint front_end_unix_socket_endpoint() const {
        return _front_end_unix_socket_endpoint;
    }

    const boost::optional<std::string>& front_end_access_token() const {
        return _front_end_access_token;
    }

    const boost::optional<std::string>& proxy_access_token() const {
        return _proxy_access_token;
    }

    boost::optional<sign::PublicKey> cache_http_pub_key() const {
        return _cache_http_pubkey;
    }

    const std::string& client_credentials() const { return _client_credentials; }

    std::string local_domain() const { return _local_domain; }

    dns::Config dns_config() const
    {
        return _dns_config;
    }

    uint64_t max_request_body_size() const {
        // The value is set in KiB in the configuration
        // and used in bytes by boost::beast
        return _max_req_body_size * 1024;
    }

    bool is_help() const { return _is_help; }

    void describe(std::ostream& os);

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

    const OuisyncClientConfig& ouisync_config() const {
        return _ouisync;
    }

private:
    boost::program_options::options_description description_full();

    // A restricted version of the above, only accepting persistent configuration options,
    // with no defaults nor descriptions.
    boost::program_options::options_description description_saved();

    void save_persistent();

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

    log_level_t log_level() const { return get_logger().get_threshold(); }
    void log_level(log_level_t level) { CHANGE_AND_SAVE_OPS(level == get_logger().get_threshold(), get_logger().set_threshold(level)); }

    const ExtraBtBsServers& bt_bootstrap_extras() const {
        return _bt_bootstrap_extras;
    }
    void bt_bootstrap_extras(ExtraBtBsServers bts) {
        CHANGE_AND_SAVE_OPS(bts == _bt_bootstrap_extras, _bt_bootstrap_extras = std::move(bts));
    }

    bool bt_bootstrap_no_default() const {
        return _bt_bootstrap_no_default;
    }

    void bt_bootstrap_no_default(bool value) {
        CHANGE_AND_SAVE_OPS(value == _bt_bootstrap_no_default, _bt_bootstrap_no_default = value);
    }

    bool bt_allow_martians() const {
        return _bt_allow_martians;
    }

    void bt_allow_martians(bool value) {
        _bt_allow_martians = value;
    }

    bool is_log_file_enabled() const { return _is_log_file_enabled(); }
    void is_log_file_enabled(bool v) { CHANGE_AND_SAVE_OPS(v == _is_log_file_enabled(), _is_log_file_enabled(v)); }

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
    bool _is_log_file_enabled() const {
        return get_logger().get_log_file() != nullptr;
    }

    void _is_log_file_enabled(bool v);

private:
    bool _is_help = false;
    fs::path _repo_root;
    fs::path _ouinet_conf_file = "ouinet-client.conf";
    fs::path _ouinet_conf_save_file = "ouinet-client.saved.conf";
    asio::ip::tcp::endpoint _local_ep;
    std::optional<uint16_t> _udp_mux_port;
    uint32_t _udp_mux_rx_limit = udp_mux_rx_limit_client;
    InjectorEndpoints _injector_endpoints;
    std::string _tls_injector_cert_path;

    std::string _tls_ca_cert_store_dir;
    std::vector<std::string> _tls_ca_cert_store_files;
    asio::ssl::context _origin_ssl_ctx{asio::ssl::context::tls_client};

    ExtraBtBsServers _bt_bootstrap_extras;
    bool _bt_bootstrap_no_default = false;
    bool _bt_allow_martians = false;
    bool _disable_origin_access = false;
    bool _disable_proxy_access = false;
    bool _disable_injector_access = false;
    asio::ip::tcp::endpoint _front_end_endpoint;
    asio::local::stream_protocol::endpoint _front_end_unix_socket_endpoint;
    boost::optional<std::string> _front_end_access_token;
    boost::optional<std::string> _proxy_access_token;
    bool _disable_bridge_announcement = false;
    EnabledCaches _enabled_caches;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
    size_t _max_simultaneous_announcements
        = default_max_simultaneous_announcements;
    uint64_t _max_req_body_size = 102400;
    bool _cache_private = false;

    std::string _client_credentials;
    std::string _injector_credentials;

    fs::path _cache_static_path;
    fs::path _cache_static_content_path;
    boost::optional<sign::PublicKey> _cache_http_pubkey;
    std::string _local_domain;
    bool _allow_private_targets = false;
    std::map<std::string, std::string> _add_request_fields;

    dns::Config _dns_config;

    std::unique_ptr<MetricsConfig> _metrics;

    size_t _i2p_hops_per_tunnel = 3;
    boost::optional<I2pAddress> _i2p_bep3_tracker;

    OuisyncClientConfig _ouisync;
};

} // ouinet namespace
