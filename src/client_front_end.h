#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/intrusive/list.hpp>
#include <chrono>
#include <cstddef>
#include <map>
#include "client.h"
#include "namespaces.h"
#include "ouiservice/bep5/client.h"
#include "ssl/ca_certificate.h"
#include "cxx/metrics.h"

namespace ouinet { namespace cache {
    class Client;
} }

namespace ouinet {

class GenericStream;
class ClientConfig;
class UPnPUpdater;
class Async;

namespace bittorrent {
class DhtBase;
}

class ClientFrontEndMetricsController {
public:
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual bool is_enabled() const = 0;

    virtual std::optional<std::string> current_record_id() const = 0;

    virtual metrics::SetAuxResult set_aux_key_value(
            std::string_view record_id,
            std::string_view key,
            std::string_view value) = 0;

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
    [[nodiscard]]
    std::expected<Response, sys::error_code>
    serve( ClientConfig&
         , const http::request<http::string_body>&
         , Client::RunningState
         , cache::Client*
         , ouiservice::Bep5Client*
         , const CACertificate&
         , const std::vector<UdpEndpoint> local_eps
         , const std::shared_ptr<UPnPs>&
         , const bittorrent::DhtBase* dht
         , ClientFrontEndMetricsController&
         , std::string_view proxy_endpoint
         , std::string_view frontend_endpoint
         , std::string_view frontend_unix_socket_endpoint
         , Async);

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

    void handle_api_groups( std::string_view
                          , const Request&
                          , Response&
                          , std::ostringstream&
                          , cache::Client*);

    std::expected<void, sys::error_code>
    handle_portal( ClientConfig&
                 , Client::RunningState
                 , const std::vector<UdpEndpoint> local_eps
                 , const std::shared_ptr<UPnPs>& upnps_ptr
                 , const bittorrent::DhtBase*
                 , const Request&
                 , Response&
                 , std::ostringstream&
                 , cache::Client*
                 , ClientFrontEndMetricsController& metrics
                 , Async);

    std::expected<void, sys::error_code>
    handle_api_status( ClientConfig&
                 , Client::RunningState
                 , const std::vector<UdpEndpoint>& local_eps
                 , const std::shared_ptr<UPnPs>&
                 , const bittorrent::DhtBase*
                 , const Request&
                 , Response&
                 , std::ostringstream&
                 , cache::Client*
                 , ouiservice::Bep5Client*
                 , ClientFrontEndMetricsController& metrics
                 , Async);

    void handle_api_metrics( std::string_view sub_path
                           , const Request&
                           , Response&
                           , std::ostringstream&
                           , ClientFrontEndMetricsController& metrics);

    static void handle_api_endpoints(std::string_view proxy_endpoint
                                   , std::string_view frontend_endpoint
                                   , std::string_view frontend_unix_socket_endpoint
                                   , Response& res, std::ostringstream& ss);

    // Enabling the log file also enables debugging temporarily.
    void enable_log_to_file(ClientConfig&);
    void disable_log_to_file(ClientConfig&);
};

} // ouinet namespace
