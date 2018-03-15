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

struct State {
    ouinet::asio::io_service ios;
    ouinet::Client client;
    thread client_thread;

    State()
        : client(ios)
    {}
};

std::unique_ptr<State> g_state;

void start_client_thread(string repo_root)
{
    if (g_state) return;

    g_state = make_unique<State>();

    {
        // Just touch this file, as the client looks into the repository and
        // fails if this conf file isn't there.
        fstream conf(repo_root + "/ouinet-client.conf", conf.binary | conf.out);
    }

    g_state->client_thread = thread([repo_root] {
            string repo_arg = "--repo=" + repo_root;

            const char* args[] = { "ouinet-client"
                                 , repo_arg.c_str()
                                 , "--listen-on-tcp=127.0.0.1:8080"
                                 , "--injector-ep=192.168.0.136:7070"
                                 };

            if (!g_state->client.start( sizeof(args) / sizeof(char*)
                                      , (char**) args))
            {
                debug("Failed to start Ouinet client");
                g_state.reset();
                return;
            }

            g_state->ios.run();
        });

}

extern "C"
JNIEXPORT void JNICALL
Java_ie_equalit_ouinet_MainActivity_startOuinetClient(
        JNIEnv *env,
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
    if (!g_state) return;
    g_state->ios.post([] { if (g_state) g_state->client.stop(); });
    g_state->client_thread.join();
    g_state.reset();
}
