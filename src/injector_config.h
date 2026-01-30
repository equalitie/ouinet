#pragma once

#include <set>

#include <boost/program_options.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/regex.hpp>
#include <boost/filesystem/path.hpp>
#include "declspec.h"
#include "constants.h"
#include "bittorrent/bootstrap.h"
#include "util/crypto.h"

namespace ouinet {

class OUINET_DECL InjectorConfig {
public:
    using ExtraBtBsServers = std::set<bittorrent::bootstrap::Address>;

    InjectorConfig() = default;
    InjectorConfig(const InjectorConfig&) = default;
    InjectorConfig(InjectorConfig&&) = default;
    InjectorConfig& operator=(const InjectorConfig&) = default;
    InjectorConfig& operator=(InjectorConfig&&) = default;

    // May thow on error.
    InjectorConfig(int argc, const char** argv);

    boost::program_options::options_description options_description();

    bool is_help() const
    { return _is_help; }

    const ExtraBtBsServers& bt_bootstrap_extras() const {
        return _bt_bootstrap_extras;
    }

    uint32_t udp_mux_rx_limit_in_bytes() const {
        // The value is set in Kbps in the configuration but required in bytes
        // by `UdpMultiplexer::maintain_max_rate_bytes_per_sec`.
        return _udp_mux_rx_limit * 1000 / 8;
    }

    boost::optional<size_t> open_file_limit() const
    { return _open_file_limit; }

    boost::filesystem::path repo_root() const
    { return _repo_root; }

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

    std::string credentials() const
    { return _credentials; }

    bool is_proxy_enabled() const
    { return !_disable_proxy; }

    boost::optional<boost::regex> target_rx() const
    { return _target_rx; }

    bool is_private_target_allowed() const
    { return _allow_private_targets; }

    bool is_doh_enabled() const
    { return !_disable_doh; }

    const std::string& tls_ca_cert_store_path() const
    { return _tls_ca_cert_store_path; }

    util::Ed25519PrivateKey cache_private_key() const
    { return _ed25519_private_key; }

private:
    void setup_ed25519_private_key(const std::string& hex);

    bool _is_http_log_file_enabled() const;

    void _is_http_log_file_enabled(bool v);

private:
    bool _is_help = false;
    boost::filesystem::path _repo_root;
    ExtraBtBsServers _bt_bootstrap_extras;
    uint32_t _udp_mux_rx_limit = udp_mux_rx_limit_injector;
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
    bool _allow_private_targets = false;
    bool _disable_doh = false;
    util::Ed25519PrivateKey _ed25519_private_key;
};

} // ouinet namespace
