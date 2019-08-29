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
ouinet::asio::io_service g_ios;
thread g_client_thread;
bool g_crypto_initialized = false;

void start_client_thread(const vector<string>& args, const vector<string>& extra_path)
{
    if (g_crypto_initialized) {
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
            catch (std::exception& e) {
                debug("Failed to start Ouinet client:");
                debug("%s", e.what());
                g_client.reset();
                return;
            }

            g_ios.run();
            debug("Ouinet's main loop stopped.");
            g_client.reset();
        });
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
    g_ios.post([] { if (g_client) g_client->stop(); });
    g_client_thread.join();
    g_client_thread = thread();
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nSetInjectorEP(
        JNIEnv* env,
        jobject /* this */,
        jstring j_injector_ep)
{
    string injector_ep = env->GetStringUTFChars(j_injector_ep, NULL);

    g_ios.post([injector_ep] {
            if (!g_client) return;
            g_client->set_injector_endpoint(injector_ep.c_str());
        });
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nSetCredentialsFor(
        JNIEnv* env,
        jobject /* this */,
        jstring j_injector,
        jstring j_credentials)
{
    string injector    = env->GetStringUTFChars(j_injector, NULL);
    string credentials = env->GetStringUTFChars(j_credentials, NULL);

    mutex m;
    unique_lock<mutex> lk(m);
    condition_variable cv;
    bool done = false;

    g_ios.post([i = move(injector), c = move(credentials), &cv, &done] {
            Defer on_exit([&] { done = true; cv.notify_one(); });
            if (!g_client) return;
            g_client->set_credentials(i.c_str(), c.c_str());
        });

    cv.wait(lk, [&]{ return done; });
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
    g_ios.post([j_is_charging] {
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
    g_ios.post([j_is_wifi_connected] {
        if (!g_client) return;
        g_client->wifi_state_change(j_is_wifi_connected);
    });
}
