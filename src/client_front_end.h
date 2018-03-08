#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <boost/intrusive/list.hpp>
#include <chrono>
#include "namespaces.h"
#include "endpoint.h"

namespace ipfs_cache { class Client; }

namespace ouinet {

class GenericConnection;

class ClientFrontEnd {
    using Clock = std::chrono::steady_clock;

    using TaskHook
        = boost::intrusive::list_base_hook
            <boost::intrusive::link_mode
                <boost::intrusive::auto_unlink>>;
public:
    using Response = http::response<http::dynamic_body>;

public:
    class Task : public TaskHook {
    public:
        Task(const std::string& name)
            : _name(name)
            , _start_time(Clock::now())
        {
            static unsigned int next_id = 0;
            _id = next_id++;
        }
        void mark_finished() { TaskHook::unlink(); }
        const std::string& name() const { return _name; }
        const Clock::duration duration() const { return Clock::now() - _start_time; }
        unsigned int id() const { return _id; }

    private:
        unsigned int _id;
        std::string _name;
        Clock::time_point _start_time;
    };

public:
    Response serve( const http::request<http::string_body>&
                  , ipfs_cache::Client*);

    bool is_injector_proxying_enabled() const
    {
        return _injector_proxying_enabled;
    }

    bool is_ipfs_cache_enabled() const
    {
        return _ipfs_cache_enabled;
    }

    Task notify_task(const std::string& task_name)
    {
        Task task(task_name);
        _pending_tasks.push_back(task);
        return task;
    }

private:
    bool _auto_refresh_enabled = true;
    bool _injector_proxying_enabled = true;
    bool _ipfs_cache_enabled = true;

    boost::intrusive::list
        < Task
        , boost::intrusive::constant_time_size<false>
        > _pending_tasks;
};

} // ouinet namespace
