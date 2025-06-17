#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <boost/intrusive/list.hpp>
#include <chrono>
//#include <ostream>
#include "client.h"
#include "namespaces.h"
#include "ssl/ca_certificate.h"
#include "util/reachability.h"
#include "util/yield.h"
#include "logger.h"

namespace ouinet { namespace cache {
    class Client;
} }

namespace ouinet {

class GenericStream;
class ClientConfig;
class UPnPUpdater;

namespace bittorrent {
class MainlineDht;
}

class ClientFrontEndMetricsController {
public:
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual bool is_enabled() const = 0;
    virtual ~ClientFrontEndMetricsController() = default;
};

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
    // Absolute paths of allowed URLs.
    static constexpr const char* log_file_apath = "/logfile.txt";
    static constexpr const char* group_list_apath = "/groups.txt";
    static constexpr const char* groups_api_path = "/api/groups";
    static constexpr const char* pinned_list_apath = "/pinned-groups.txt";

public:
    using Request = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;
    using UdpEndpoint = asio::ip::udp::endpoint;
    using UPnPs = std::map<UdpEndpoint, std::unique_ptr<UPnPUpdater>>;

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
                  , Client::RunningState
                  , cache::Client*
                  , const CACertificate&
                  , boost::optional<UdpEndpoint> local_ep
                  , const UPnPs&
                  , const bittorrent::MainlineDht* dht
                  , const util::UdpServerReachabilityAnalysis*
                  , ClientFrontEndMetricsController&
                  , Cancel
                  , Yield yield);

    Task notify_task(const std::string& task_name)
    {
        Task task(task_name);
        _pending_tasks.push_back(task);
        return task;
    }

    ClientFrontEnd(const ClientConfig&);
    ~ClientFrontEnd();

private:
    bool _auto_refresh_enabled = false;
    bool _show_pending_tasks = false;
    boost::optional<log_level_t> _log_level_no_file;

    std::unique_ptr<Input<log_level_t>> _log_level_input;

    boost::intrusive::list
        < Task
        , boost::intrusive::constant_time_size<false>
        > _pending_tasks;

    void handle_ca_pem( const Request&, Response&, std::ostringstream&
                      , const CACertificate& );

    void handle_group_list( const Request&
                          , Response&
                          , std::ostringstream&
                          , cache::Client*);

    void handle_pinned_list( const Request&
                          , Response&
                          , std::ostringstream&
                          , cache::Client*);

    void handle_groups( const Request&
                      , Response&
                      , std::ostringstream&
                      , cache::Client*
                      , Cancel cancel
                      , Yield);

    void handle_portal( ClientConfig&
                      , Client::RunningState
                      , boost::optional<UdpEndpoint> local_ep
                      , const UPnPs&
                      , const bittorrent::MainlineDht*
                      , const util::UdpServerReachabilityAnalysis*
                      , const Request&
                      , Response&
                      , std::ostringstream&
                      , cache::Client*
                      , ClientFrontEndMetricsController& metrics
                      , Cancel cancel
                      , Yield);

    void handle_status( ClientConfig&
                      , Client::RunningState
                      , boost::optional<UdpEndpoint> local_ep
                      , const UPnPs&
                      , const bittorrent::MainlineDht*
                      , const util::UdpServerReachabilityAnalysis*
                      , const Request&
                      , Response&
                      , std::ostringstream&
                      , cache::Client*
                      , ClientFrontEndMetricsController& metrics
                      , Cancel cancel
                      , Yield);

    // Enabling the log file also enables debugging temporarily.
    void enable_log_to_file(ClientConfig&);
    void disable_log_to_file(ClientConfig&);
};

} // ouinet namespace
