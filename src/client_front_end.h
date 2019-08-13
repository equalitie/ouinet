#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <boost/intrusive/list.hpp>
#include <chrono>
//#include <ostream>
#include "namespaces.h"
#include "ssl/ca_certificate.h"
#include "util/yield.h"
#include "logger.h"

namespace ouinet { namespace bep44_ipfs {
    class CacheClient;
} }

namespace ouinet {

class GenericStream;
class ClientConfig;

class ClientFrontEnd {
    template<typename E> struct Input;

    template<typename E>
        friend std::ostream& operator<<(std::ostream&, const Input<E>&);

    using Clock = std::chrono::steady_clock;

    using TaskHook
        = boost::intrusive::list_base_hook
            <boost::intrusive::link_mode
                <boost::intrusive::auto_unlink>>;
public:
    using Request = http::request<http::string_body>;
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
    Response serve( ClientConfig&
                  , const http::request<http::string_body>&
                  , bep44_ipfs::CacheClient*, const CACertificate&
                  , Yield yield);

    Task notify_task(const std::string& task_name)
    {
        Task task(task_name);
        _pending_tasks.push_back(task);
        return task;
    }

    ClientFrontEnd();
    ~ClientFrontEnd();

private:
    bool _auto_refresh_enabled = true;
    bool _show_pending_tasks = false;

    std::unique_ptr<Input<log_level_t>> _log_level_input;

    boost::intrusive::list
        < Task
        , boost::intrusive::constant_time_size<false>
        > _pending_tasks;

    void handle_ca_pem( const Request&, Response&, std::stringstream&
                      , const CACertificate& );

    void handle_upload( const ClientConfig&
                      , const Request&, Response&, std::stringstream&
                      , bep44_ipfs::CacheClient*, asio::yield_context);

    void handle_descriptor( const ClientConfig&
                          , const Request&, Response&, std::stringstream&
                          , bep44_ipfs::CacheClient*, Yield);

    void handle_insert_bep44(const Request&, Response&, std::stringstream&
                            , bep44_ipfs::CacheClient*, asio::yield_context);

    void handle_portal( ClientConfig&
                      , const Request&
                      , Response&
                      , std::stringstream&
                      , bep44_ipfs::CacheClient*);

    void handle_status( ClientConfig&
                      , const Request&
                      , Response&
                      , std::stringstream&
                      , bep44_ipfs::CacheClient*);
};

} // ouinet namespace
