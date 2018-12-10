#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <map>
#include <string>
#include "../or_throw.h"
#include "../util/signal.h"

namespace ouinet {

enum class DbType { btree, bep44 };

static const std::map<DbType, std::string> DbName = {
    {DbType::btree, "Btree"},
    {DbType::bep44, "BEP44"}
};

class ClientDb {
public:
    virtual std::string find(const std::string& key, Cancel&, asio::yield_context) = 0;

    // Insert a signed key->descriptor mapping.
    // The parsing of the given data depends on the data base.
    // Return a printable representation of the key resulting from insertion.
    virtual std::string insert_mapping(const std::string&, asio::yield_context yield) {
        return or_throw<std::string>(yield, asio::error::operation_not_supported);
    };
};

class InjectorDb : public ClientDb {
public:
    // May set `asio::error::message_size` if the value is too big
    // to be stored directly in the data base.
    // The returned string depends on the implementation and
    // it should help an untrusted agent reinsert the key->value mapping into the data base
    // (e.g. by including protocol-dependent signature data).
    virtual std::string insert(std::string key, std::string value, asio::yield_context) = 0;
};

} // namespace
