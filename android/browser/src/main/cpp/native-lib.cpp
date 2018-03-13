#include <jni.h>
#include <string>
#include <android/log.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <redirect_to_android_log.h>

using namespace std;

thread client_thread;

int run_client(int argc, char* argv[]);

void start_client(string repo_root)
{
    {
        // Just touch this file, as the client looks into the repository and
        // fails if this conf file isn't there.
        fstream conf(repo_root + "/ouinet-client.conf", conf.binary | conf.out);
    }

    client_thread = thread([repo_root] {
            ouinet::RedirectToAndroidLog cout_guard(cout);
            ouinet::RedirectToAndroidLog cerr_guard(cerr);

            string repo_arg = "--repo=" + repo_root;

            const char* args[] = { "ouinet-client"
                                 , repo_arg.c_str()
                                 , "--listen-on-tcp=127.0.0.1:8080"
                                 , "--injector-ep=192.168.0.136:7070"
                                 };

            int status = run_client( sizeof(args) / sizeof(char*)
                                   , (char**) args);

            cerr << "Ouinet returned: " << status << endl;
        });

}

extern "C"
JNIEXPORT jstring JNICALL
Java_ie_equalit_ouinet_MainActivity_startOuinetClient(
        JNIEnv *env,
        jobject /* this */,
        jstring repo_root) {

    __android_log_print(ANDROID_LOG_VERBOSE, "Ouinet", "startOuinetClient 1");

    const char* path = env->GetStringUTFChars(repo_root, NULL);

    start_client(path);

    __android_log_print(ANDROID_LOG_VERBOSE, "Ouinet", "startOuinetClient 2");

    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}
