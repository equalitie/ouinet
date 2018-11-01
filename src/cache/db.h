#pragma once

#include <boost/asio/spawn.hpp>
#include <string>
#include "../util/signal.h"

namespace ouinet {

enum class DbType { btree, bep44 };

class ClientDb {
public:
    virtual std::string find(const std::string& key, Cancel&, asio::yield_context) = 0;
};

class InjectorDb : public ClientDb {
public:
    // May set `asio::error::message_size` if the value is too big
    // to be stored directly in the data base.
    virtual void insert(std::string key, std::string value, asio::yield_context) = 0;
};

} // namespace
