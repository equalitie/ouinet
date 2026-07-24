#pragma once

#include "session.h"
#include "route.h"
#include "request.h"
#include "namespaces.h"

#include <variant>
#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/dynamic_body.hpp>

namespace ouinet {

class CacheControl;

class Dispatcher {
public:
    template<class V> using SysResult = std::expected<V, sys::error_code>;

    using Request  = http::request<http::string_body>;

    class Response {
    public:
        struct FrontEnd { http::response<http::dynamic_body> value; };
        struct Origin { Session session; };
        struct DCache { CacheRequest request; Session session; };
        struct LocalCache { Session session; };
        struct PublicInjector { CacheRequest request; Session session; };
        struct PrivateInjector { Session session; };
        struct Ouisync { Session session; };

        using Alternatives = std::variant<
            FrontEnd,
            Origin,
            DCache,
            LocalCache,
            PublicInjector,
            PrivateInjector,
            Ouisync
        >;

        template<class Rs>
        requires(
            !std::is_same_v<Response, Rs> &&
            std::constructible_from<Alternatives, Rs>
        )
        Response(Rs&& rs): value(std::forward<Rs>(rs)) {}

        Response(Response&&) = default;
        Response(Response const&) = delete;
        Response& operator=(Response&& other) = default;
        Response& operator=(Response const&) = delete;

        http::response_header<> const& header() const;

        [[nodiscard]]
        SysResult<void> write(GenericStream& stream, Async yield);

        bool keep_alive() const;

        Alternatives value;
    };

    struct Routes {
        [[nodiscard]]
        virtual SysResult<decltype(Response::FrontEnd::value)>
        front_end(const Request&, Async) = 0;
    
        [[nodiscard]]
        virtual SysResult<Session>
        origin(const Request&, Async) = 0;

        [[nodiscard]]
        virtual SysResult<Session>
        public_injector(const CacheInjectRequest&, Async) = 0;
    
        [[nodiscard]]
        virtual SysResult<Session>
        private_injector(const Request&, Async) = 0;

        [[nodiscard]]
        virtual SysResult<Session>
        distributes_cache(const CacheRetrieveRequest&, Async) = 0;

        virtual boost::posix_time::time_duration max_cached_age() = 0;

        virtual bool is_injector_starting() = 0;
    };

public:
    Dispatcher(asio::any_io_executor exec, Routes& routes);

    [[nodiscard]]
    SysResult<Response>
    dispatch(const Request& request, const Route& route, Async yield);

private:
    template<class PrimaryJob, class SecondaryJob>
    [[nodiscard]]
    SysResult<Response>
    primary_or_secondary(PrimaryJob, SecondaryJob, auto secondary_delay, Async);

    SysResult<Response> fetch_from_front_end(Request const&, Async);
    SysResult<Response> fetch_from_origin(Request const&, Async);
    SysResult<Response> fetch_from_dcache(CacheType, Request const&, Async);
    SysResult<Response> fetch_from_public_injector(CacheType, Request const&, Async);
    SysResult<Response> fetch_from_private_injector(CacheType, Request const&, Async);
    SysResult<Response> fetch_from_cache_control(CacheType, Request const&, Async);

private:
    Routes& routes;
    std::unique_ptr<CacheControl> cache_control;
};

} // namespace
