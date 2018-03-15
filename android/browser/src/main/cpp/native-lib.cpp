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
    State() : client(ios) {}
};

// g_state is only accessed from the g_client_thread.
std::unique_ptr<State> g_state;
thread g_client_thread;

void start_client_thread(string repo_root)
{
    if (g_client_thread.get_id() != thread::id()) return;

    g_client_thread = thread([repo_root] {
            if (g_state) return;
            g_state = make_unique<State>();

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
                                 , "--injector-ep=192.168.0.136:7070"
                                 };

            unsigned argc = sizeof(args) / sizeof(char*);

            try {
                g_state->client.start(argc, (char**) args);
            }
            catch (std::exception& e) {
                debug("Failed to start Ouinet client:");
                debug("%s", e.what());
                g_state.reset();
                return;
            }

            g_state->ios.run();
            g_state.reset();
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
    g_state->ios.post([] {
            if (g_state) g_state->client.stop();
        });
    g_client_thread.join();
    g_client_thread = thread();
}
