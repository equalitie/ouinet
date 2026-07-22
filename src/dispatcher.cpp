#include <chrono>
#include "dispatcher.h"
#include "cache_control.h"
#include "constants.h"
#include "util/select.h"
#include "util/wait_condition.h"
#include "util/overloaded.h"
#include "logger.h"

namespace ouinet {

using Response = Dispatcher::Response;
template<class V> using SysResult = Dispatcher::SysResult<V>;


Dispatcher::Dispatcher(asio::any_io_executor exec, Routes& routes)
    : routes(routes)
    , cache_control(std::make_unique<CacheControl>(exec, OUINET_CLIENT_SERVER_STRING))
{
    cache_control->fetch_fresh = [&] (const CacheInjectRequest& rq, Async yield) {
        return routes.public_injector(rq, yield);
    };

    cache_control->fetch_stored = [&] (const CacheRetrieveRequest& rq, Async yield) {
        return routes.distributes_cache(rq, yield);
    };

    cache_control->max_cached_age(routes.max_cached_age());
}


template<class PrimaryJob, class SecondaryJob>
SysResult<Response>
Dispatcher::primary_or_secondary(PrimaryJob primary, SecondaryJob secondary, auto secondary_delay, Async main_yield)
{
    using R1 = std::invoke_result_t<PrimaryJob, Async>;
    using R2 = std::invoke_result_t<SecondaryJob, Async>;

    std::optional<R1> primary_r;
    std::optional<R2> secondary_r;

    auto exec = main_yield.get_executor();
    WaitCondition wc(exec);
    WaitCondition primary_wc(exec);

    auto task_yield = main_yield;

    task_yield.spawn([&, &out_r = primary_r, &task = primary, lock = wc.lock(), p_lock = primary_wc.lock()] (Async yield) {
        try {
            out_r = task(yield);
            if (*out_r) task_yield.cancel();
        }
        catch (Async::Cancelled const&) {
            if (main_yield.is_cancelled()) throw;
            out_r = std::unexpected(asio::error::operation_aborted);
        }
    });

    task_yield.spawn([&, &out_r = secondary_r, &task = secondary, lock = wc.lock()] (Async yield) {
        try {
            timeout(secondary_delay, [&] (Async yield) { return primary_wc.wait(yield); }, yield);
            out_r = task(yield);
            if (*out_r) task_yield.cancel();
        }
        catch (Async::Cancelled const&) {
            if (main_yield.is_cancelled()) throw;
            out_r = std::unexpected(asio::error::operation_aborted);
        }
    });

    wc.wait(main_yield);

    if (*primary_r) return std::move(*primary_r);
    if (*secondary_r) return std::move(*secondary_r);

    return std::unexpected(primary_r->error());
}


template<class W, class T>
static SysResult<W> wrap(SysResult<T> value) {
    if (!value) return std::unexpected(value.error());
    return W(std::move(*value));
}

template<class W, class T>
static SysResult<W> wrap(CacheRequest rq, SysResult<T> value) {
    if (!value) return std::unexpected(value.error());
    return W(std::move(rq), std::move(*value));
}


SysResult<Response>
Dispatcher::dispatch(const Request& request, const Route& route, Async yield) {
    using namespace std::chrono_literals;

    using R = SysResult<Response>;

    return std::visit(overloaded {
            [&] (Route::FrontEnd const&) -> R {
                return fetch_from_front_end(request, yield);
            },
            [&] (Route::Origin const&) -> R {
                return fetch_from_origin(request, yield);
            },
            [&] (Route::BlindInjector const&) -> R {
                return fetch_from_private_injector(request, yield);
            },
            [&] (Route::OriginOrBlindInjector const&) -> R {
                return primary_or_secondary(
                        [&] (Async yield) {
                            return fetch_from_origin(request, yield);
                        },
                        [&] (Async yield) {
                            return fetch_from_private_injector(request, yield);
                        },
                        3s,
                        yield);
            },
            [&] (Route::PublicInjector const&) -> R {
                return fetch_from_public_injector(request, yield);
            },
            [&] (Route::DCache const&) -> R {
                return fetch_from_dcache(request, yield);
            },
            [&] (Route::OriginOrPublicInjectorOrDCache const&) -> R {
                return primary_or_secondary(
                        [&] (Async yield) {
                            return fetch_from_origin(request, yield);
                        },
                        [&] (Async yield) {
                            return fetch_from_cache_control(request, yield);
                        },
                        routes.is_injector_starting() ? 1s : 3s,
                        yield);
            },
            [&] (Route::OriginOrDCache const&) -> R {
                return primary_or_secondary(
                        [&] (Async yield) -> R {
                            return fetch_from_origin(request, yield);
                        },
                        [&] (Async yield) -> R {
                            return fetch_from_dcache(request, yield);
                        },
                        routes.is_injector_starting() ? 1s : 3s,
                        yield);
            },
            [&] (Route::OriginOrPublicInjector const&) -> R {
                return primary_or_secondary(
                        [&] (Async yield) -> R {
                            return fetch_from_origin(request, yield);
                        },
                        [&] (Async yield) -> R {
                            return fetch_from_public_injector(request, yield);
                        },
                        routes.is_injector_starting() ? 1s : 3s,
                        yield);
            },
            [&] (Route::PublicInjectorOrDCache const&) -> R {
                return fetch_from_cache_control(request, yield);
            },
        },
        route.value);
}


SysResult<Response> Dispatcher::fetch_from_front_end(Request const& request, Async yield) {
    auto r = routes.front_end(request, yield);
    return wrap<Response::FrontEnd>(std::move(r));
}

SysResult<Response> Dispatcher::fetch_from_origin(Request const& request, Async yield) {
    auto r = routes.origin(request, yield);
    return wrap<Response::Origin>(std::move(r));
}

SysResult<Response> Dispatcher::fetch_from_dcache(Request const& request, Async yield) {
    const auto cache_rq = CacheRequest::from(request);
    if (!cache_rq) {
        LOG_ERROR(yield, " Invalid request");
        return std::unexpected(asio::error::invalid_argument);
    }
    auto r = routes.distributes_cache(cache_rq->to_retrieve_request(), yield);
    return wrap<Response::DCache>(std::move(*cache_rq), std::move(r));
}

SysResult<Response> Dispatcher::fetch_from_public_injector(Request const& request, Async yield) {
    const auto cache_rq = CacheRequest::from(request);
    if (!cache_rq) {
        LOG_ERROR(yield, " Invalid request");
        return std::unexpected(asio::error::invalid_argument);
    }
    auto r = routes.public_injector(cache_rq->to_inject_request(), yield);
    return wrap<Response::PublicInjector>(std::move(*cache_rq), std::move(r));
}

SysResult<Response> Dispatcher::fetch_from_private_injector(Request const& request, Async yield) {
    auto r = routes.private_injector(request, yield);
    return wrap<Response::PrivateInjector>(std::move(r));
}

SysResult<Response> Dispatcher::fetch_from_cache_control(Request const& request, Async yield) {
    const auto cache_rq = CacheRequest::from(request);
    if (!cache_rq) {
        LOG_ERROR(yield, " Invalid request");
        return std::unexpected(asio::error::invalid_argument);
    }

    auto r = cache_control->fetch(*cache_rq, yield);

    if (!r) return std::unexpected(r.error());

    auto& response_hdr = r->response_header();
    auto source = response_hdr[http_::response_source_hdr];

    if (source == http_::response_source_hdr_dist_cache) {
        return wrap<Response::DCache>(std::move(*cache_rq), std::move(r));
    }
    if (source == http_::response_source_hdr_injector) {
        return wrap<Response::PublicInjector>(std::move(*cache_rq), std::move(r));
    }
    if (source == http_::response_source_hdr_local_cache) {
        return wrap<Response::LocalCache>(std::move(r));
    }
    if (source == http_::response_source_hdr_ouisync) {
        return wrap<Response::Ouisync>(std::move(r));
    }

    LOG_ERROR(yield, " Response from CacheControl has an invalid source header: \"", source, "\"");
    assert(false);
    // TODO: Better error, or even better have CacheControl return typed responses
    // to make this path unreachable by the compiler.
    return std::unexpected(asio::error::fault);
}


inline
std::variant<
    const http::response<http::dynamic_body>*,
    const Session*
>
get_inner(const Dispatcher::Response& response) {
    using R = std::variant<
        const http::response<http::dynamic_body>*,
        const Session*
    >;

    return std::visit(overloaded {
            [] (const Response::FrontEnd& v) -> R { return &v.value; },
            [] (const Response::Origin& v) -> R { return &v.session; },
            [] (const Response::DCache& v) -> R { return &v.session; },
            [] (const Response::LocalCache& v) -> R { return &v.session; },
            [] (const Response::PublicInjector& v) -> R { return &v.session; },
            [] (const Response::PrivateInjector& v) -> R { return &v.session; },
            [] (const Response::Ouisync& v) -> R { return &v.session; }
       },
       response.value);
}

inline
std::variant<
    http::response<http::dynamic_body>*,
    Session*
>
get_inner(Dispatcher::Response& response) {
    using R = std::variant<
        http::response<http::dynamic_body>*,
        Session*
    >;

    return std::visit(overloaded {
            [] (Response::FrontEnd& v) -> R { return &v.value; },
            [] (Response::Origin& v) -> R { return &v.session; },
            [] (Response::DCache& v) -> R { return &v.session; },
            [] (Response::LocalCache& v) -> R { return &v.session; },
            [] (Response::PublicInjector& v) -> R { return &v.session; },
            [] (Response::PrivateInjector& v) -> R { return &v.session; },
            [] (Response::Ouisync& v) -> R { return &v.session; }
       },
       response.value);
}

http::response_header<> const& Dispatcher::Response::header() const {
    using R = http::response_header<>;
    return std::visit(overloaded {
            [&] (Session const* session) -> R const& {
                return session->response_header();
            },
            [&] (auto const* rs) -> R const& {
                return *rs;
            }
        },
        get_inner(*this));
}

[[nodiscard]]
SysResult<void> Dispatcher::Response::write(GenericStream& stream, Async yield) {
    using R = std::expected<void, sys::error_code>;
    return std::visit(overloaded {
            [&] (Session* session) -> R {
                return session->flush_response(stream, yield, PartModifier::RemoveChunkHeaderExtension);
            },
            [&] (http::response<http::dynamic_body>* rs) -> R {
                auto r = http::async_write(stream, *rs, yield);
                if (!r) return std::unexpected(r.error());
                return {};
            }
        },
        get_inner(*this));
}

bool Dispatcher::Response::keep_alive() const {
    return std::visit(overloaded {
            [&] (Session* session) {
                return session->response_header().keep_alive();
            },
            [&] (auto* rs) {
                return rs->keep_alive();
            }
        },
        get_inner(*this));
}

} // namespace
