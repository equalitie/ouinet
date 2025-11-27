#pragma once

// OUINET_DECL_EXPORT is defined in CMakeLists.txt when building the client library.
#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CODEGEARC__) || defined(__MINGW32__)
#    if defined(OUINET_DECL_EXPORT)
#        define OUINET_DECL __declspec(dllexport)
#    else
#        define OUINET_DECL __declspec(dllimport)
#    endif
#else
#    define OUINET_DECL
#endif

