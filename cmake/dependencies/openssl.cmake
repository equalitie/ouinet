include(ExternalProject)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    set(OPENSSL_VERSION "1.1.1q")

    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(OPENSSL_TARGET "android-arm")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(OPENSSL_TARGET "android-arm64")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(OPENSSL_TARGET "android-x86")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(OPENSSL_TARGET "android-x86_64")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    # openssl does not compile with __ANDROID_API__ past a certain point.
    # Presumably this will get fixed in a future openssl version.
    # For now, defining an old version seems to work.
    # Please read `doc/android-sdk-versions.md` and keep in sync with it.
    if (${ANDROID_PLATFORM_LEVEL} LESS $ENV{OUINET_MIN_API})
        set(OPENSSL_ANDROID_VERSION ${ANDROID_PLATFORM_LEVEL})
    else()
        set(OPENSSL_ANDROID_VERSION $ENV{OUINET_MIN_API})
    endif()

    set(BUILT_OPENSSL_VERSION ${OPENSSL_VERSION})
    set(BUILT_OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/include)
    set(BUILT_OPENSSL_SSL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(BUILT_OPENSSL_CRYPTO_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX})

    externalproject_add(built_openssl
        URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
        URL_HASH SHA256=d7939ce614029cdff0b6c20f0e2e5703158a489a72b2507b8bd51bf8c8fd10ca
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && ./Configure
                ${OPENSSL_TARGET}
                no-shared -no-ssl2 -no-ssl3 -no-comp -no-hw -no-engine
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/openssl/install
                -D__ANDROID_API__=${OPENSSL_ANDROID_VERSION}
        BUILD_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && make depend
            && make build_libs
        BUILD_BYPRODUCTS
            ${BUILT_OPENSSL_SSL_LIBRARY}
            ${BUILT_OPENSSL_CRYPTO_LIBRARY}
        INSTALL_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && make install_dev
    )

    set(OpenSSL_DIR ${CMAKE_CURRENT_LIST_DIR}/inline-openssl)
    list(INSERT CMAKE_MODULE_PATH 0 ${OpenSSL_DIR})
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    set(OPENSSL_VERSION "1.1.1q")

    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    if (${PLATFORM} STREQUAL "OS64")
        set(OPENSSL_TARGET "ios64-xcrun")
    elseif (${PLATFORM} STREQUAL "SIMULATOR64")
        set(OPENSSL_TARGET "darwin64-x86_64-cc")
    elseif (${PLATFORM} STREQUAL "SIMULATORARM64")
        set(OPENSSL_TARGET "iossimulator-xcrun")
    endif()

    set(BUILT_OPENSSL_VERSION ${OPENSSL_VERSION})
    set(BUILT_OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/include)
    set(BUILT_OPENSSL_SSL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(BUILT_OPENSSL_CRYPTO_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX})

    externalproject_add(built_openssl
        URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
        URL_HASH SHA256=d7939ce614029cdff0b6c20f0e2e5703158a489a72b2507b8bd51bf8c8fd10ca
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && ./Configure
                ${OPENSSL_TARGET}
                no-shared -no-dso -no-hw -no-engine -fembed-bitcode
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/openssl/install
        BUILD_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && make depend
            && make build_libs
        BUILD_BYPRODUCTS
            ${BUILT_OPENSSL_SSL_LIBRARY}
            ${BUILT_OPENSSL_CRYPTO_LIBRARY}
        INSTALL_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && make install_dev
    )

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
