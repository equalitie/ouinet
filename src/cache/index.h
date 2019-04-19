#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/utility/string_view.hpp>
#include <map>
#include <string>
#include "../or_throw.h"
#include "../util/signal.h"

namespace ouinet {

enum class IndexType { btree, bep44 };

static const std::map<IndexType, std::string> IndexName = {
    {IndexType::btree, "Btree"},
    {IndexType::bep44, "BEP44"}
};

class ClientIndex {
public:
    // If set, when the index detects a change in an entry that this client is publishing,
    // this function is called with the old and new values in the index,
    // and it returns whether it considers the new value
    // usable for further processing (e.g. storage or publishing).
    // It should *not* propagate an error code.
    using UpdatedHook = std::function<bool( std::string old, std::string new_
                                          , Cancel&, asio::yield_context) noexcept>;
    virtual void updated_hook(UpdatedHook) {
        throw std::logic_error("not implemented");
    };

    virtual std::string find(const std::string& key, Cancel&, asio::yield_context) = 0;

    // Insert a signed key->descriptor mapping.
    // The parsing of the given data depends on the index.
    // Return a printable representation of the key resulting from insertion.
    virtual std::string insert_mapping( const boost::string_view target
                                      , const std::string&
                                      , Cancel&
                                      , asio::yield_context yield) {
        return or_throw<std::string>(yield, asio::error::operation_not_supported);
    };
};

class InjectorIndex : public ClientIndex {
public:
    // May set `asio::error::message_size` if the value is too big
    // to be stored directly in the index.
    // The returned string depends on the implementation and
    // it should help an untrusted agent reinsert the key->value mapping into the index
    // (e.g. by including protocol-dependent signature data).
    virtual std::string insert( std::string key
                              , std::string value
                              , asio::yield_context) = 0;

    // Same as above, but don't do any IO, only return the same string
    virtual std::string get_insert_message( std::string key
                                          , std::string value
                                          , sys::error_code& ec) {
        // Only some indices support this operation
        ec = asio::error::operation_not_supported;
        return std::string();
    }
};

} // namespace
