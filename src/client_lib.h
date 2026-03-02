#pragma once

#ifdef __cplusplus
#  define OUINET_CLIENT_EXTERN_C  extern "C"
#else
#  define OUINET_CLIENT_EXTERN_C
#endif

// Taken from https://gcc.gnu.org/wiki/Visibility#Step-by-step_guide
#if defined _WIN32 || defined __CYGWIN__
  #define OUINET_CLIENT_DLL_IMPORT __declspec(dllimport)
  #define OUINET_CLIENT_DLL_EXPORT __declspec(dllexport)
  #define OUINET_CLIENT_DLL_LOCAL
#else
  #if __GNUC__ >= 4
    #define OUINET_CLIENT_DLL_IMPORT __attribute__ ((visibility ("default")))
    #define OUINET_CLIENT_DLL_EXPORT __attribute__ ((visibility ("default")))
    #define OUINET_CLIENT_DLL_LOCAL  __attribute__ ((visibility ("hidden")))
  #else
    #define OUINET_CLIENT_DLL_IMPORT
    #define OUINET_CLIENT_DLL_EXPORT
    #define OUINET_CLIENT_DLL_LOCAL
  #endif
#endif

#if defined(OUINET_DECL_EXPORT)
    // Library for Windows exposes OuinetClient symbols
    #define OUINET_CLIENT_LIBRARY_API OUINET_CLIENT_DLL_EXPORT
#elif defined(OUINET_DECL_INTERNAL)
    // Android and iOS uses OuinetClient from within the same library
    #define OUINET_CLIENT_LIBRARY_API OUINET_CLIENT_DLL_LOCAL
#else
    // Header consumed by external binary. Import symbols
    #define OUINET_CLIENT_LIBRARY_API OUINET_CLIENT_DLL_IMPORT
#endif

// returns 0 on success
OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
int ouinet_client_run(int argc, const char *argv[], void(*on_exit_callback)(int));

// Supply 0 to return without waiting for ouinet to finish
// Subsequent ouinet_client_run will block until the previous run is complete
OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
void ouinet_client_stop(int block_until_client_is_stopped);

OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
const char* ouinet_client_get_error();

OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
int ouinet_client_get_client_state();

OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
const char* ouinet_client_get_proxy_endpoint();

OUINET_CLIENT_EXTERN_C OUINET_CLIENT_LIBRARY_API
const char* ouinet_client_get_frontend_endpoint();

#undef OUINET_CLIENT_EXTERN_C
#undef OUINET_CLIENT_DLL_IMPORT
#undef OUINET_CLIENT_DLL_EXPORT
#undef OUINET_CLIENT_DLL_LOCAL
#undef OUINET_CLIENT_LIBRARY_API
