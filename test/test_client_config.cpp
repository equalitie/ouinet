#define BOOST_TEST_MODULE utility
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "client_config.h"
#include "util/test_dir.h"

using namespace std;
using namespace ouinet;
using namespace std::string_literals;

// Look into rust/record_format.md for information on how to generate this.
static string public_key_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VuAyEAdrkFffyZjr5r6k1Jl2+27fv0KvJu+H8Xk7GwjKnRiHc=\n"
    "-----END PUBLIC KEY-----";

static ClientConfig make_config(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv),
                   [](const std::string& s) { return s.c_str(); });
    return ClientConfig(argv.size(), argv.data());
}

static MetricsServerConfig make_server(int priority, const std::string& url) {
    MetricsServerConfig conf;
    conf.url = *util::Url::from(url);
    conf.priority = priority;
    return conf;
}

BOOST_AUTO_TEST_SUITE(ouinet_metrics_server_priority)

// --- group_servers_by_priority -----------------------------------------

BOOST_AUTO_TEST_CASE(group_by_priority_empty) {
    std::vector<MetricsServerConfig> servers;
    auto tiers = group_servers_by_priority(servers);
    BOOST_CHECK(tiers.empty());
}

BOOST_AUTO_TEST_CASE(group_by_priority_single_tier) {
    std::vector<MetricsServerConfig> servers;
    servers.push_back(make_server(0, "http://a.example.com"));
    servers.push_back(make_server(0, "http://b.example.com"));
    servers.push_back(make_server(0, "http://c.example.com"));

    auto tiers = group_servers_by_priority(servers);

    BOOST_REQUIRE_EQUAL(tiers.size(), 1u);
    BOOST_REQUIRE_EQUAL(tiers[0].size(), 3u);
    // Servers with equal priority keep their relative order.
    BOOST_CHECK_EQUAL(tiers[0][0], &servers[0]);
    BOOST_CHECK_EQUAL(tiers[0][1], &servers[1]);
    BOOST_CHECK_EQUAL(tiers[0][2], &servers[2]);
}

BOOST_AUTO_TEST_CASE(group_by_priority_multiple_tiers_ordered_ascending) {
    std::vector<MetricsServerConfig> servers;
    servers.push_back(make_server(5, "http://low-a.example.com"));
    servers.push_back(make_server(0, "http://high-a.example.com"));
    servers.push_back(make_server(5, "http://low-b.example.com"));
    servers.push_back(make_server(0, "http://high-b.example.com"));
    servers.push_back(make_server(2, "http://mid.example.com"));

    auto tiers = group_servers_by_priority(servers);

    // Lowest priority value first (highest priority tier), in insertion order
    // within a tier.
    BOOST_REQUIRE_EQUAL(tiers.size(), 3u);

    BOOST_REQUIRE_EQUAL(tiers[0].size(), 2u);
    BOOST_CHECK_EQUAL(tiers[0][0], &servers[1]);
    BOOST_CHECK_EQUAL(tiers[0][1], &servers[3]);

    BOOST_REQUIRE_EQUAL(tiers[1].size(), 1u);
    BOOST_CHECK_EQUAL(tiers[1][0], &servers[4]);

    BOOST_REQUIRE_EQUAL(tiers[2].size(), 2u);
    BOOST_CHECK_EQUAL(tiers[2][0], &servers[0]);
    BOOST_CHECK_EQUAL(tiers[2][1], &servers[2]);
}

// --- --metrics-server-priority config parsing ---------------------------

BOOST_AUTO_TEST_CASE(priority_defaults_when_option_omitted) {
    TestDir test_dir;

    auto config = make_config({
        "./no_client_exec"s,
        "--repo"s, test_dir.string(),
        "--metrics-server-url"s, "http://metrics.example.com/ingest"s,
        "--metrics-encryption-key"s, public_key_pem,
    });

    BOOST_REQUIRE(config.metrics());
    BOOST_REQUIRE_EQUAL(config.metrics()->servers.size(), 1u);
    BOOST_CHECK_EQUAL(config.metrics()->servers[0].priority, default_metrics_server_priority);
}

BOOST_AUTO_TEST_CASE(priority_parsed_per_server_in_order) {
    TestDir test_dir;

    auto config = make_config({
        "./no_client_exec"s,
        "--repo"s, test_dir.string(),
        "--metrics-server-url"s, "http://a.example.com/ingest"s,
        "--metrics-server-url"s, "http://b.example.com/ingest"s,
        "--metrics-server-priority"s, "5"s,
        "--metrics-server-priority"s, "1"s,
        "--metrics-encryption-key"s, public_key_pem,
    });

    BOOST_REQUIRE(config.metrics());
    BOOST_REQUIRE_EQUAL(config.metrics()->servers.size(), 2u);
    BOOST_CHECK_EQUAL(config.metrics()->servers[0].priority, 5);
    BOOST_CHECK_EQUAL(config.metrics()->servers[1].priority, 1);
}

BOOST_AUTO_TEST_CASE(priority_count_must_match_url_count) {
    TestDir test_dir;

    BOOST_CHECK_THROW(
        make_config({
            "./no_client_exec"s,
            "--repo"s, test_dir.string(),
            "--metrics-server-url"s, "http://a.example.com/ingest"s,
            "--metrics-server-url"s, "http://b.example.com/ingest"s,
            "--metrics-server-priority"s, "1"s,
            "--metrics-encryption-key"s, public_key_pem,
        }),
        std::exception
    );
}

BOOST_AUTO_TEST_CASE(priority_requires_server_url) {
    TestDir test_dir;

    BOOST_CHECK_THROW(
        make_config({
            "./no_client_exec"s,
            "--repo"s, test_dir.string(),
            "--metrics-server-priority"s, "1"s,
        }),
        std::exception
    );
}

BOOST_AUTO_TEST_SUITE_END()
