#pragma once

#include "ouiservice/i2p/service.h"
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include "api.h"

namespace ouinet {

OUINET_COMMON_API
void add_i2p_service_options(boost::program_options::options_description&);

// May throw
OUINET_COMMON_API
std::optional<I2pService::Config> parse_i2p_service_config(
        boost::program_options::variables_map const&,
        const fs::path& repo_root);

} // namespace
