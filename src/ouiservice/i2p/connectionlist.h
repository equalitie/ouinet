#pragma once

#include <boost/intrusive/list.hpp>

#include "connection.h"

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class ConnectionList {
public:
    void add(Connection& connection)
    {
        _connections.push_back(connection);
    }

    void close_all()
    {
        auto connections = std::move(_connections);
        for (auto& connection : connections) {
            connection.close();
        }
    }

private:
    boost::intrusive::list<Connection, boost::intrusive::constant_time_size<false>> _connections;
};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
