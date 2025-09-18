#include <jni.h>
#include <string>
#include <android/log.h>
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

#include "debug.h"
#include "std_scoped_redirect.h"

using namespace std;
using ouinet::ClientConfig;

struct Defer {
    Defer(function<void()> f) : _f(move(f)) {}
    ~Defer() { _f(); }
    function<void()> _f;
};

// g_client is only accessed from the g_client_thread.
std::unique_ptr<ouinet::Client> g_client;
ouinet::asio::io_context g_ctx;
thread g_client_thread;
bool g_crypto_initialized = false;

void start_client_thread(const vector<string>& args, const vector<string>& extra_path)
{
    if (!g_crypto_initialized) {
        ouinet::util::crypto_init();
        g_crypto_initialized = true;
    }

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

    if (g_client_thread.get_id() != thread::id()) return;

    g_client_thread = thread([=] {
            if (g_client) return;

            StdScopedRedirect redirect_guard;

            debug("Starting new ouinet client.");

            // In case we're restarting.
            g_ctx.restart();

            vector<const char*> args_;
            args_.reserve(args.size());

            for (const auto& arg : args) {
                args_.push_back(arg.c_str());
            }

            try {
                ClientConfig cfg(args_.size(), (char**) args_.data());
                g_client = make_unique<ouinet::Client>(g_ctx, move(cfg));
                g_client->start();
            }
            catch (const std::exception& e) {
                debug("Failed to start Ouinet client:");
                debug("%s", e.what());
                g_client.reset();
                return;
            }

            try {
                g_ctx.run();
            }
            catch (const std::exception& e) {
                debug("Exception thrown from ouinet");
                debug("%s", e.what());
            }

            debug("Ouinet's main loop stopped.");
            g_client.reset();
        });
}

extern "C"
JNIEXPORT jint JNICALL
Java_ie_equalit_ouinet_Ouinet_nGetClientState(
        JNIEnv* env,
        jobject /* this */)
{
    // TODO: Avoid needing to keep this in sync by hand.
    if (!g_client)
        return g_ctx.stopped() ? 6 /* stopped */ : 0 /* created */;
    switch (g_client->get_state()) {
    case ouinet::Client::RunningState::Created:  return 0;
    case ouinet::Client::RunningState::Failed:   return 1;
    case ouinet::Client::RunningState::Starting: return 2;
    case ouinet::Client::RunningState::Degraded: return 3;
    case ouinet::Client::RunningState::Started:  return 4;
    case ouinet::Client::RunningState::Stopping: return 5;
    case ouinet::Client::RunningState::Stopped:  return 6;
    }
    return -1;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_ie_equalit_ouinet_Ouinet_nGetProxyEndpoint(
        JNIEnv* env,
        jobject /* this */)
{
    if (!g_client)
        return env->NewStringUTF("");
    return env->NewStringUTF(g_client->get_proxy_endpoint().c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_ie_equalit_ouinet_Ouinet_nGetFrontendEndpoint(
        JNIEnv* env,
        jobject /* this */)
{
    if (!g_client)
        return env->NewStringUTF("");
    return env->NewStringUTF(g_client->get_frontend_endpoint().c_str());
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nStartClient(
        JNIEnv* env,
        jobject /* this */,
        jobjectArray jargs,
        jobjectArray jpath)
{
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

    start_client_thread(args, path);
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nStopClient(
        JNIEnv *env,
        jobject /* this */)
{
    try {
      if (g_client_thread.get_id() == thread::id()) return;
      ouinet::asio::post(g_ctx, [] { if (g_client) g_client->stop(); });
      g_client_thread.join();
      g_client_thread = thread();
    } catch (const std::exception &e) {
      debug("Failed to stop Ouinet client:");
      debug("%s", e.what());
      return;
    }
}

extern "C"
JNIEXPORT jstring JNICALL
Java_ie_equalit_ouinet_Ouinet_nGetCARootCert(
        JNIEnv* env,
        jclass /* class */,
        jstring j_repo_root)
{
    string repo_root = env->GetStringUTFChars(j_repo_root, NULL);
    return env->NewStringUTF(ouinet::Client::get_or_gen_ca_root_cert(repo_root).c_str());
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nChargingStateChange(
        JNIEnv* env,
        jobject /* this */,
        jboolean j_is_charging) {
    ouinet::asio::post(g_ctx, [j_is_charging] {
        if (!g_client) return;
        g_client->charging_state_change(j_is_charging);
    });
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nWifiStateChange(
        JNIEnv* env,
        jobject /* this */,
        jboolean j_is_wifi_connected) {
    ouinet::asio::post(g_ctx, [j_is_wifi_connected] {
        if (!g_client) return;
        g_client->wifi_state_change(j_is_wifi_connected);
    });
}
