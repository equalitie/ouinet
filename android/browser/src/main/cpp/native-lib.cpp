#include <jni.h>
#include <string>
#include <android/log.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <boost/asio.hpp>

#include <redirect_to_android_log.h>
#include <namespaces.h>
#include <util/signal.h>

using namespace std;

#define debug(...) __android_log_print(ANDROID_LOG_VERBOSE, "Ouinet", __VA_ARGS__);

struct State {
    ouinet::Signal<void()> shutdown_signal;
    ouinet::asio::io_service ios;
    thread client_thread;
};

std::unique_ptr<State> g_state;

// TODO: This should be in a header
void call_shutdown_signal(ouinet::Signal<void()>&);

// TODO: This should be in a header
int start_client( ouinet::asio::io_service& ios
                , ouinet::Signal<void()>& shutdown_signal
                , int argc
                , char* argv[]);

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
            ouinet::RedirectToAndroidLog cout_guard(cout);
            ouinet::RedirectToAndroidLog cerr_guard(cerr);

            string repo_arg = "--repo=" + repo_root;

            const char* args[] = { "ouinet-client"
                                 , repo_arg.c_str()
                                 , "--listen-on-tcp=127.0.0.1:8080"
                                 , "--injector-ep=192.168.0.136:7070"
                                 };

            if (!start_client( g_state->ios
                             , g_state->shutdown_signal
                             , sizeof(args) / sizeof(char*)
                             , (char**) args))
            {
                cerr << "Failed to start Ouinet client" << endl;
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
    g_state->ios.post([] {
            call_shutdown_signal(g_state->shutdown_signal);
         });
    g_state->client_thread.join();
    g_state.reset();
}
