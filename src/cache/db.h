#pragma once

#include <boost/asio/spawn.hpp>
#include <string>

namespace ouinet {

enum class DbType { btree, bep44 };

class ClientDb {
public:
    virtual std::string find(const std::string& key, asio::yield_context) = 0;
};

class InjectorDb : public ClientDb {
public:
    virtual void insert(std::string key, std::string value, asio::yield_context) = 0;
};

} // namespace
