#pragma once

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CODEGEARC__) || defined(__MINGW32__)
#   define OUINET_USES_API
#endif

#ifdef OUINET_USES_API
#    if defined(OUINET_API_COMMON_LOCAL)
#        define OUINET_COMMON_API
#    elif defined(OUINET_API_COMMON_EXPORT)
#        define OUINET_COMMON_API __declspec(dllexport)
#    else
#        define OUINET_COMMON_API __declspec(dllimport)
#    endif
#else
#    define OUINET_COMMON_API
#endif

#ifdef OUINET_USES_API
#    if defined(OUINET_API_INJECTOR_LOCAL)
#        define OUINET_INJECTOR_API
#    elif defined(OUINET_API_INJECTOR_EXPORT)
#        define OUINET_INJECTOR_API __declspec(dllexport)
#    else
#        define OUINET_INJECTOR_API __declspec(dllimport)
#    endif
#else
#    define OUINET_INJECTOR_API
#endif

#ifdef OUINET_USES_API
#    if defined(OUINET_API_CLIENT_LOCAL)
#        define OUINET_CLIENT_API
#    elif defined(OUINET_API_CLIENT_EXPORT)
#        define OUINET_CLIENT_API __declspec(dllexport)
#    else
#        define OUINET_CLIENT_API __declspec(dllimport)
#    endif
#else
#    define OUINET_CLIENT_API
#endif

#ifdef OUINET_USES_API
#    if defined(OUINET_API_OUISYNC_LOCAL)
#        define OUINET_OUISYNC_API
#    elif defined(OUINET_API_OUISYNC_EXPORT)
#        define OUINET_OUISYNC_API __declspec(dllexport)
#    else
#        define OUINET_OUISYNC_API __declspec(dllimport)
#    endif
#else
#    define OUINET_OUISYNC_API
#endif

#ifdef OUINET_USES_API
#    if defined(OUINET_API_I2P_LOCAL)
#        define OUINET_I2P_API
#    elif defined(OUINET_API_I2P_EXPORT)
#        define OUINET_I2P_API __declspec(dllexport)
#    else
#        define OUINET_I2P_API __declspec(dllimport)
#    endif
#else
#    define OUINET_I2P_API
#endif
