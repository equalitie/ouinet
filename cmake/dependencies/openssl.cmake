include(ExternalProject)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    set(OPENSSL_VERSION "1.1.1c")

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(HOSTTRIPLE "arm-linux-androideabi")
        set(OPENSSL_TARGET "android-arm")
    elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm.*")
        # Is this still relevant? armv<7 seems to be obsolete
        # from android 4.4 onwards.
        set(HOSTTRIPLE "armv5te-linux-androideabi")
        set(OPENSSL_TARGET "android-arm")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(HOSTTRIPLE "aarch64-linux-android")
        set(OPENSSL_TARGET "android-arm64")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(HOSTTRIPLE "i686-linux-android")
        set(OPENSSL_TARGET "android-x86")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(HOSTTRIPLE "x86_64-linux-android")
        set(OPENSSL_TARGET "android-x86_64")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    externalproject_add(built_openssl
        URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
        URL_MD5 15e21da6efe8aa0e0768ffd8cd37a5f6
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        # The openssl android support is _amazingly_ broken. The patch below
        # fixes it for this particular version and this particular configuration.
        # In any other setting, who the hell knows.
        PATCH_COMMAND
            sed -i -e "s:/\\$sysroot:/sysroot:"
                ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl/Configurations/15-android.conf
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export CROSS_COMPILE="${HOSTTRIPLE}-"
            && export PATH=${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}
            && ./Configure
                ${OPENSSL_TARGET}
                no-shared -no-ssl2 -no-ssl3 -no-comp -no-hw -no-engine
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/openssl/install
        BUILD_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export CROSS_COMPILE="${HOSTTRIPLE}-"
            && export PATH=${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}
            && make depend
            && make build_libs
        INSTALL_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export PATH=${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}
            && make install_dev
    )

    set(BUILT_OPENSSL_VERSION ${OPENSSL_VERSION})
    set(BUILT_OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/include)
    set(BUILT_OPENSSL_SSL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(BUILT_OPENSSL_CRYPTO_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX})

    set(OpenSSL_DIR ${CMAKE_CURRENT_LIST_DIR}/inline-openssl)
    list(INSERT CMAKE_MODULE_PATH 0 ${OpenSSL_DIR})
endif()

find_package(OpenSSL REQUIRED)

# Some container classes rely on openssl headers, but not on linking to
# the library proper.
if (NOT TARGET OpenSSL::headers)
    add_library(openssl_headers INTERFACE)
    add_library(OpenSSL::headers ALIAS openssl_headers)
    target_include_directories(openssl_headers
        INTERFACE "${OPENSSL_INCLUDE_DIR}"
    )
    if (TARGET built_openssl)
        add_dependencies(openssl_headers built_openssl)
    endif()
endif()
