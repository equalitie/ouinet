#include "config/util.h"
#include "config/i2p_service.h"
#include "parse/endpoint.h"
#include <boost/filesystem.hpp>

namespace ouinet {

namespace po = boost::program_options;

void add_i2p_service_options(boost::program_options::options_description& dsc) {
    dsc.add_options()
       ("enable-i2p-service-ext"
        , po::value<std::string>()
        , "Attempt to connect to an external I2P service on the given endpoint")
       ("enable-i2p-service-exe"
        , po::value<fs::path>()
        , "Attempt to start `i2pd` executable at the given path")
       ("enable-i2p-service-lib"
        , po::bool_switch()->default_value(false)
        , "Attempt to start `i2pd` in the same process")
       ("managed-i2pd-sam-port"
        , po::value<uint16_t>()->default_value(7656)
        , "SAM TCP port for the i2pd instance managed by ouinet (lib or exe); "
          "0 will always leads to portUDP=65535 due to a bug in i2pd SAMBridge")
       ;
}

std::optional<I2pService::Config> parse_i2p_service_config(po::variables_map const& vm, const fs::path& repo_root) {
    I2pService::Config config;

    // ConfigExternal
    if (auto ext_endpoint = as_optional<std::string>(vm, "enable-i2p-service-ext")) {
        auto ep = parse::endpoint<asio::ip::tcp>(*ext_endpoint);
        if (!ep) {
            throw error("Failed to parse --enable-i2p-service-ext: ", *ext_endpoint);
        }
        config.ext = I2pService::ConfigExternal { *ep };
    }

    auto datadir = repo_root / "i2pd";

    auto sam_port = vm["managed-i2pd-sam-port"].as<uint16_t>();
    bool sam_port_explicit = !vm["managed-i2pd-sam-port"].defaulted();

    // ConfigI2pdExe
    if (auto path = as_optional<fs::path>(vm, "enable-i2p-service-exe")) {
        boost::filesystem::create_directories(datadir);
        config.i2pd_exe = I2pService::ConfigI2pdExe { *path, datadir, sam_port };
    }
    
    // ConfigI2pdLib
    if (vm["enable-i2p-service-lib"].as<bool>()) {
        boost::filesystem::create_directories(datadir);
        config.i2pd_lib = I2pService::ConfigI2pdLib { datadir, sam_port };
    }

    if (sam_port_explicit && !config.i2pd_lib && !config.i2pd_exe) {
        throw error("--managed-i2pd-sam-port requires --enable-i2p-service-lib or --enable-i2p-service-exe");
    }

    if (!config.ext && !config.i2pd_exe && !config.i2pd_lib) {
        return {};
    }

    return config;
}

} // namespace
