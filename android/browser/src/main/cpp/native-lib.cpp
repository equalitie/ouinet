#include <jni.h>
#include <string>
#include <android/log.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <boost/asio.hpp>
#include <namespaces.h>
#include <client.h>
#include <util/signal.h>

using namespace std;

#define debug(...) __android_log_print(ANDROID_LOG_VERBOSE, "Ouinet", __VA_ARGS__);

// g_client is only accessed from the g_client_thread.
std::unique_ptr<ouinet::Client> g_client;
ouinet::asio::io_service g_ios;
thread g_client_thread;

void start_client_thread(string repo_root)
{
    if (g_client_thread.get_id() != thread::id()) return;

    g_client_thread = thread([repo_root] {
            if (g_client) return;

            g_client = make_unique<ouinet::Client>(g_ios);

            // In case we're restarting.
            g_ios.reset();

            {
                // Just touch this file, as the client looks into the
                // repository and fails if this conf file isn't there.
                fstream conf(repo_root + "/ouinet-client.conf"
                            , conf.binary | conf.out);
            }

            string repo_arg = "--repo=" + repo_root;

            const char* args[] = { "ouinet-client"
                                 , repo_arg.c_str()
                                 , "--listen-on-tcp=127.0.0.1:8080"
                                 };

            unsigned argc = sizeof(args) / sizeof(char*);

            try {
                g_client->start(argc, (char**) args);
            }
            catch (std::exception& e) {
                debug("Failed to start Ouinet client:");
                debug("%s", e.what());
                g_client.reset();
                return;
            }

            g_ios.run();
            g_client.reset();
        });
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_MainActivity_startOuinetClient(
        JNIEnv* env,
        jobject /* this */,
        jstring repo_root)
{
    const char* path = env->GetStringUTFChars(repo_root, NULL);
    start_client_thread(path);
}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_MainActivity_stopOuinetClient(
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
Java_ie_equalit_ouinet_MainActivity_setOuinetInjectorEP(
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
Java_ie_equalit_ouinet_MainActivity_setOuinetIPNS(
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
