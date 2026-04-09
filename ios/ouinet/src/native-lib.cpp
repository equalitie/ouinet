#include "native-lib.hpp"
#include <TargetConditionals.h>

#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <client.h>
#include <client_config.h>
#include <util/signal.h>
#include <util/crypto.h>
#include <condition_variable>
#include <future>
#include <mutex>
#include <atomic>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

//#include "debug.h"
//#include "std_scoped_redirect.h"

using namespace std;
using ouinet::ClientConfig;

/*
struct Defer {
    Defer(function<void()> f) : _f(move(f)) {}
    ~Defer() { _f(); }
    function<void()> _f;
};
*/

// g_client is only accessed from the g_client_thread.
std::unique_ptr<ouinet::Client> g_client;
ouinet::asio::io_context g_ctx;
thread g_client_thread;
bool g_crypto_initialized = false;

// Thread-safe cached values for cross-thread access
std::atomic<bool> g_client_ready{false};
std::string g_proxy_endpoint_cached;
std::string g_frontend_endpoint_cached;
std::mutex g_endpoint_mutex;

void start_client_thread(const std::vector<std::string>& args) //, const vector<string>& extra_path)
{
    if (!g_crypto_initialized) {
        ouinet::util::crypto_init();
        g_crypto_initialized = true;
    }

    /*
    char* old_path_c = getenv("PATH");
    if (old_path_c) {
        std::string old_path(old_path_c);
        std::set<std::string> old_path_entries;
        size_t index = 0;
        while (true) {
            size_t pos = old_path.find(':', index);
            if (pos == std::string::npos) {
                old_path_entries.insert(old_path.substr(index));
                break;
            } else {
                old_path_entries.insert(old_path.substr(index, pos - index));
                index = pos + 1;
            }
        }

        std::string new_path;
        for (size_t i = 0; i < extra_path.size(); i++) {
            if (!old_path_entries.count(extra_path[i])) {
                new_path += extra_path[i];
                new_path += ":";
            }
        }
        new_path += old_path;
        setenv("PATH", new_path.c_str(), 1);
    }
    */

    // Already running — nothing to do.
    if (g_client_ready) return;

    // If the previous thread finished but was never joined, join it now
    // so that assigning a new thread won't call std::terminate().
    if (g_client_thread.joinable()) {
        g_client_thread.join();
    }

    std::cout<<"Ouinet config:"<<std::endl;
    for (std::string arg: args) {
        std::cout<<arg<<std::endl;
    }

    g_client_thread = thread([=] {
            if (g_client) return;

            //StdScopedRedirect redirect_guard;

            //debug("Starting new ouinet client.");
            std::cout<<"Starting new ouinet client"<<std::endl;
            LOG_WARN("OUINET::Starting new ouinet client");

            // In case we're restarting.
            g_ctx.restart();

            vector<const char*> args_;
            args_.reserve(args.size());

            for (const auto& arg : args) {
                args_.push_back(arg.c_str());
            }

            try {
                ClientConfig cfg(args_.size(), const_cast<const char**>(args_.data()));
                g_client = make_unique<ouinet::Client>(g_ctx, move(cfg));
                g_client->start();

                // Cache endpoints for thread-safe access
                {
                    std::lock_guard<std::mutex> lock(g_endpoint_mutex);
                    auto ep = g_client->get_proxy_endpoint();
                    g_proxy_endpoint_cached = ep.address().to_string() + ":" + to_string(ep.port());
                    g_frontend_endpoint_cached = g_client->get_frontend_endpoint();
                }
                g_client_ready = true;
            }
            catch (const std::exception& e) {
                //debug("Failed to start Ouinet client:");
                //debug("%s", e.what());
                std::cout<<"Failed to start ouinet client"<<std::endl;
                std::cout<<e.what()<<std::endl;
                g_client.reset();
                return;
            }

            try {
                g_ctx.run();
            }
            catch (const std::exception& e) {
                //debug("Exception thrown from ouinet");
                //debug("%s", e.what());
                std::cout<<"Exception thrown from ouinet"<<std::endl;
                std::cout<<e.what()<<std::endl;
            }

            //debug("Ouinet's main loop stopped.");
            std::cout<<"Ouinet's main loop stopped."<<std::endl;
            g_client_ready = false;
            {
                std::lock_guard<std::mutex> lock(g_endpoint_mutex);
                g_proxy_endpoint_cached.clear();
                g_frontend_endpoint_cached.clear();
            }
            g_client.reset();
        });
}

int NativeLib::getClientState()
{
    // TODO: Avoid needing to keep this in sync by hand.
    if (!g_client_ready)
        return g_ctx.stopped() ? 6 /* stopped */ : -1 /* missing */;

    // Post to io_context thread and wait for result
    std::promise<int> promise;
    ouinet::asio::post(g_ctx, [&promise] {
        if (!g_client) {
            promise.set_value(-1);
            return;
        }
        switch (g_client->get_state()) {
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

void NativeLib::startClient(const std::vector<std::string>& args)//, const vector<string>& extra_path)
{
    /*
    size_t argn = env->GetArrayLength(jargs);
    vector<string> args;
    args.reserve(argn);

    for (size_t i = 0; i < argn; ++i) {
        jstring jstr = (jstring) env->GetObjectArrayElement(jargs, i);
        const char* arg = env->GetStringUTFChars(jstr, 0);
        args.push_back(arg);
        env->ReleaseStringUTFChars(jstr, arg);
    }


    size_t pathn = env->GetArrayLength(jpath);
    vector<string> path;
    path.reserve(pathn);

    for (size_t i = 0; i < pathn; ++i) {
        jstring jstr = (jstring) env->GetObjectArrayElement(jpath, i);
        const char* dir = env->GetStringUTFChars(jstr, 0);
        path.push_back(dir);
        env->ReleaseStringUTFChars(jstr, dir);
    }
    */
    std::cout<<"Starting ouinet client"<<std::endl;
    start_client_thread(args);
}

void NativeLib::stopClient()
{
    if (!g_client_thread.joinable()) return;

    // Post stop() to io_context thread to ensure thread-safe access
    ouinet::asio::post(g_ctx, [] {
        if (g_client) g_client->stop();
    });

    g_client_thread.join();
    g_client_thread = thread();
    // Note: g_client.reset() happens inside the thread after g_ctx.run() returns
}

std::string NativeLib::helloOuinet()
{
    return std::string("Hello Ouinet, this libary was definitely compiled inside of the ouinet cmake build system, cool");
}


std::string NativeLib::getProxyEndpoint() const noexcept {
    if (!g_client_ready) return "";
    std::lock_guard<std::mutex> lock(g_endpoint_mutex);
    return g_proxy_endpoint_cached;
}

std::string NativeLib::getFrontendEndpoint() const noexcept {
    if (!g_client_ready) return "";
    std::lock_guard<std::mutex> lock(g_endpoint_mutex);
    return g_frontend_endpoint_cached;
}