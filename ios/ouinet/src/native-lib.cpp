#include "native-lib.hpp"
#include <TargetConditionals.h>

#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <client.h>
#include <client_config.h>
#include <util/signal.h>
#include <util/crypto.h>
#include <future>
#include <mutex>
#include <optional>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;
using ouinet::ClientConfig;

// ── Per-run session ────────────────────────────────────────────────────────────
// All state belonging to a single start/stop cycle.
// Shared between g_current_session and the detached worker thread via shared_ptr,
// so the object is kept alive until both sides release it.
struct ClientSession {
    ouinet::asio::io_context ctx;
    std::unique_ptr<ouinet::Client> client;
    std::optional<ouinet::asio::executor_work_guard<
        ouinet::asio::io_context::executor_type>> ctx_guard;
};

// ── Globals ───────────────────────────────────────────────────────────────────

// g_session_mutex protects all of: g_current_session, g_proxy_endpoint_cached,
// g_frontend_endpoint_cached.
std::shared_ptr<ClientSession> g_current_session;
std::string g_proxy_endpoint_cached;
std::string g_frontend_endpoint_cached;
std::mutex g_session_mutex;

// ── Session cleanup ────────────────────────────────────────────────────────────
// Called from the detached thread after ctx.run() exits (normally or via exception).
// Clears global state only if this session is still the active one — prevents
// a finishing old session from clobbering a newly started session's state.
static void on_session_exit(const std::shared_ptr<ClientSession>& session)
{
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        if (g_current_session.get() == session.get()) {
            g_current_session = nullptr;
            g_proxy_endpoint_cached.clear();
            g_frontend_endpoint_cached.clear();
        }
    }

    // Always release session's own resources regardless of whether current
    session->ctx_guard.reset();
    session->client.reset();
}

// ── start_client_thread ────────────────────────────────────────────────────────
// Called under g_session_mutex from startClient().
void start_client_thread(const std::vector<std::string>& args)
{
    static std::once_flag crypto_init_flag;
    std::call_once(crypto_init_flag, ouinet::util::crypto_init);

    if (g_current_session) return;  // session already active — starting or running

    auto session = std::make_shared<ClientSession>();
    g_current_session = session;  // protected — caller holds g_session_mutex

    std::cout << "Ouinet config:" << std::endl;
    for (const std::string& arg : args) std::cout << arg << std::endl;

    std::thread([session, args]() {
        std::cout << "Starting new ouinet client" << std::endl;
        LOG_WARN("OUINET::Starting new ouinet client");

        vector<const char*> args_;
        args_.reserve(args.size());
        for (const auto& arg : args) args_.push_back(arg.c_str());

        try {
            ClientConfig cfg(args_.size(), const_cast<const char**>(args_.data()));
            session->client = make_unique<ouinet::Client>(session->ctx, move(cfg));
            session->client->start();

            {
                std::lock_guard<std::mutex> lock(g_session_mutex);
                auto ep = session->client->get_proxy_endpoint();
                g_proxy_endpoint_cached = ep.address().to_string() + ":" + to_string(ep.port());
                g_frontend_endpoint_cached = session->client->get_frontend_endpoint();
            }
        }
        catch (const std::exception& e) {
            std::cout << "Failed to start ouinet client: " << e.what() << std::endl;
            on_session_exit(session);
            return;
        }

        session->ctx_guard.emplace(session->ctx.get_executor());
        try {
            session->ctx.run();
        }
        catch (const std::exception& e) {
            std::cout << "Exception thrown from ouinet: " << e.what() << std::endl;
        }

        std::cout << "Ouinet's main loop stopped." << std::endl;
        on_session_exit(session);
        // session shared_ptr released here — object destroyed if stopClient
        // already released its ref via the posted stop handler
    }).detach();
}

// ── NativeLib ─────────────────────────────────────────────────────────────────
int NativeLib::getClientState()
{
    std::shared_ptr<ClientSession> session;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        session = g_current_session;
    }
    if (!session || session->ctx.stopped()) return -1;

    std::promise<int> promise;
    ouinet::asio::post(session->ctx, [&promise, session] {
        if (!session->client) { promise.set_value(-1); return; }
        switch (session->client->get_state()) {
        case ouinet::Client::RunningState::Created:  promise.set_value(0); break;
        case ouinet::Client::RunningState::Failed:   promise.set_value(1); break;
        case ouinet::Client::RunningState::Starting: promise.set_value(2); break;
        case ouinet::Client::RunningState::Degraded: promise.set_value(3); break;
        case ouinet::Client::RunningState::Started:  promise.set_value(4); break;
        case ouinet::Client::RunningState::Stopping: promise.set_value(5); break;
        case ouinet::Client::RunningState::Stopped:  promise.set_value(6); break;
        default: promise.set_value(-1); break;
        }
    });
    return promise.get_future().get();
}

void NativeLib::startClient(const std::vector<std::string>& args)
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    std::cout << "Starting ouinet client" << std::endl;
    start_client_thread(args);
}

void NativeLib::stopClient()
{
    std::shared_ptr<ClientSession> session;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        session = std::move(g_current_session);
        g_current_session = nullptr;  // new startClient() can proceed immediately
    }

    if (!session) return;

    // Post stop into the session's own io_context — returns immediately.
    // The detached thread holds the last shared_ptr ref and will clean up.
    ouinet::asio::post(session->ctx, [session]() mutable {
        if (session->client) session->client->stop();
        session->ctx_guard.reset();  // allows ctx.run() to return once work drains
    });
}

std::string NativeLib::helloOuinet()
{
    return std::string("Hello Ouinet, this libary was definitely compiled inside of the ouinet cmake build system, cool");
}

std::string NativeLib::getProxyEndpoint() const noexcept {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    return g_proxy_endpoint_cached;
}

std::string NativeLib::getFrontendEndpoint() const noexcept {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    return g_frontend_endpoint_cached;
}
