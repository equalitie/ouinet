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
#include <util/signal.h>
#include <condition_variable>

#include <fcntl.h>
#include <unistd.h>

#include "debug.h"
#include "std_scoped_redirect.h"

using namespace std;

struct Defer {
    Defer(function<void()> f) : _f(move(f)) {}
    ~Defer() { _f(); }
    function<void()> _f;
};

// g_client is only accessed from the g_client_thread.
std::unique_ptr<ouinet::Client> g_client;
ouinet::asio::io_service g_ios;
thread g_client_thread;

void start_client_thread( string repo_root
                        , string injector_ep
                        , string ipns
                        , string credentials
                        , bool enable_http_connect_requests)
{
    if (g_client_thread.get_id() != thread::id()) return;

    g_client_thread = thread([=] {
            if (g_client) return;

            StdScopedRedirect redirect_guard;

            debug("Starting new ouinet client.");

            g_client = make_unique<ouinet::Client>(g_ios);

            // In case we're restarting.
            g_ios.reset();

            {
                // Just touch this file, as the client looks into the
                // repository and fails if this conf file isn't there.
                fstream conf( repo_root + "/ouinet-client.conf"
                            , conf.binary | conf.out);
            }

            string repo_arg        = "--repo="                 + repo_root;
            string injector_ep_arg = "--injector-ep="          + injector_ep;
            string ipns_arg        = "--injector-ipns="        + ipns;
            string credentials_arg = "--injector-credentials=" + credentials;

            vector<const char*> args;

            args.push_back("ouinet-client");
            args.push_back("--listen-on-tcp=127.0.0.1:8080");
            args.push_back("--front-end-ep=0.0.0.0:8081");
            args.push_back(repo_arg.c_str());

            if (!injector_ep.empty()) {
                args.push_back(injector_ep_arg.c_str());
            }

            if (!ipns.empty()) {
                args.push_back(ipns_arg.c_str());
            }

            if (!credentials.empty()) {
                args.push_back(credentials_arg.c_str());
            }

            if (enable_http_connect_requests) {
                args.push_back("--enable-http-connect-requests");
            }

            try {
                g_client->start(args.size(), (char**) args.data());
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
        jstring j_repo_root,
        jstring j_injector_ep,
        jstring j_ipns,
        jstring j_credentials,
        jboolean enable_http_connect_requests)
{
    const char* repo_root   = env->GetStringUTFChars(j_repo_root,   NULL);
    const char* injector_ep = env->GetStringUTFChars(j_injector_ep, NULL);
    const char* ipns        = env->GetStringUTFChars(j_ipns,        NULL);
    const char* credentials = env->GetStringUTFChars(j_credentials, NULL);

    start_client_thread( repo_root
                       , injector_ep
                       , ipns
                       , credentials
                       , enable_http_connect_requests);
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_Ouinet_nStopClient(
        JNIEnv *env,
        jobject /* this */,
        jstring repo_root)
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
Java_ie_equalit_ouinet_Ouinet_nSetIPNS(
        JNIEnv* env,
        jobject /* this */,
        jstring j_inps)
{
    string ipns = env->GetStringUTFChars(j_inps, NULL);

    g_ios.post([ipns] {
            if (!g_client) return;
            g_client->set_ipns(ipns.c_str());
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
