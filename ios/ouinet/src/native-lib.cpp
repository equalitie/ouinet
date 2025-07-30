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
ouinet::asio::io_service g_ios;
thread g_client_thread;
bool g_crypto_initialized = false;

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

    if (g_client_thread.get_id() != thread::id()) return;

    std::cout<<"Ouinet config:"<<std::endl;
    for (std::string arg: args) {
        std::cout<<arg<<std::endl;
    }

    g_client_thread = thread([=] {
            if (g_client) return;

            //StdScopedRedirect redirect_guard;

            //debug("Starting new ouinet client.");
            std::cout<<"Starting new ouinet client"<<std::endl;

            // In case we're restarting.
            g_ios.reset();

            vector<const char*> args_;
            args_.reserve(args.size());

            for (const auto& arg : args) {
                args_.push_back(arg.c_str());
            }

            try {
                ClientConfig cfg(args_.size(), (char**) args_.data());
                g_client = make_unique<ouinet::Client>(g_ios, move(cfg));
                g_client->start();
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
                g_ios.run();
            }
            catch (const std::exception& e) {
                //debug("Exception thrown from ouinet");
                //debug("%s", e.what());
                std::cout<<"Exception thrown from ouinet"<<std::endl;
                std::cout<<e.what()<<std::endl;
            }

            //debug("Ouinet's main loop stopped.");
            std::cout<<"Ouinet's main loop stopped."<<std::endl;
            g_client.reset();
        });
}

int NativeLib::getClientState()
{
    // TODO: Avoid needing to keep this in sync by hand.
    if (!g_client)
        return g_ios.stopped() ? 6 /* stopped */ : -1 /* missing */;
    switch (g_client->get_state()) {
    case ouinet::Client::RunningState::Created:  return 0;
    case ouinet::Client::RunningState::Failed:   return 1;
    case ouinet::Client::RunningState::Starting: return 2;
    case ouinet::Client::RunningState::Degraded: return 3;
    case ouinet::Client::RunningState::Started:  return 4;
    case ouinet::Client::RunningState::Stopping: return 5;
    case ouinet::Client::RunningState::Stopped:  return 6;
    }
    return -1 /* missing */;
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

std::string NativeLib::helloOuinet()
{
    return std::string("Hello Ouinet, this libary was definitely compiled inside of the ouinet cmake build system, cool");
}
