#pragma once

#include <functional>

#include <boost/intrusive/list.hpp>

namespace ouinet {

template<typename T>
class Signal {
private:
    using Hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

public:
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    class Connection : public Hook
    {
        friend class Signal;
        std::function<T> slot;
    };

    template<typename... Args>
    void operator()(Args&&... args)
    {
        auto connections = std::move(_connections);
        for (auto& connection : connections) {
            connection.slot(std::forward<Args>(args)...);
        }
    }

    Connection connect(std::function<T> slot)
    {
        Connection connection;
        connection.slot = std::move(slot);
        _connections.push_back(connection);
        return connection;
    }

private:
    boost::intrusive::list<Connection, boost::intrusive::constant_time_size<false>> _connections;
};

} // ouinet namespace
