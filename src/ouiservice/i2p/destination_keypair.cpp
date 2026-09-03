#include "destination_keypair.h"
#include "namespaces.h"
#include "logger.h"
#include <boost/json.hpp>

namespace json = boost::json;

namespace ouinet {

std::string I2pDestinationKeypair::to_json_string() const {
    return json::serialize(json::value {
        { "public", pub },
        { "private", priv },
    });
}

std::optional<I2pDestinationKeypair> I2pDestinationKeypair::from_json_string(std::string_view str) {
    sys::error_code ec;
    json::value jv = json::parse(str, ec);

    if (ec) {
        LOG_ERROR("Failed to parse I2pDestinationKeypair as json: ", ec.message());
        return {};
    }

    try {
        return I2pDestinationKeypair {
            std::string(jv.at("public").as_string()),
            std::string(jv.at("private").as_string())
        };
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to read I2pDestinationKeypair json values: ", e.what());
        return {};
    }
}

} // namespace
