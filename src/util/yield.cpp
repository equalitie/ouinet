#include "yield.h"

namespace ouinet {

void YieldContext::log(boost::string_view str)
{
    YieldContext::log(INFO, str);
}

void YieldContext::log(log_level_t log_level, boost::string_view str)
{
    using boost::string_view;

    if (logger.get_threshold() > log_level)
        return;

    while (str.size()) {
        auto endl = str.find('\n');

        logger.log(log_level, util::str(_log_path, " ", str.substr(0, endl)));

        if (endl == std::string::npos) {
            break;
        }

        str = str.substr(endl+1);
    }
}

} // namespace
