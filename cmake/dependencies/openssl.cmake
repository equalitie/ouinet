include(ExternalProject)

if (NOT "${CMAKE_GENERATOR}" STREQUAL "Ninja" AND NOT "${CMAKE_GENERATOR}" STREQUAL "Xcode" AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    # Ninja doesn't support these.
    # The job server options were introduced in CMake v3.28.0
    set(BUILD_JOB_SERVER_AWARE BUILD_JOB_SERVER_AWARE YES)
    set(INSTALL_JOB_SERVER_AWARE INSTALL_JOB_SERVER_AWARE YES)
endif()

set(OPENSSL_VERSION_Android "1.1.1q")
set(OPENSSL_VERSION_iOS     "1.1.1q")
set(OPENSSL_VERSION_Windows "3.6.0")
set(OPENSSL_VERSION ${OPENSSL_VERSION_${CMAKE_SYSTEM_NAME}})

if (DEFINED OPENSSL_VERSION)
    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(OPENSSL_URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz")

    set(OPENSSL_URL_HASH_1.1.1q SHA256=d7939ce614029cdff0b6c20f0e2e5703158a489a72b2507b8bd51bf8c8fd10ca)
    set(OPENSSL_URL_HASH_3.4.1  SHA256=002a2d6b30b58bf4bea46c43bdd96365aaf8daa6c428782aa4feee06da197df3)
    set(OPENSSL_URL_HASH ${OPENSSL_URL_HASH_${OPENSSL_VERSION}})

    set(OPENSSL_TARGET_Android_armv7-a    "android-arm")
    set(OPENSSL_TARGET_Android_aarch64    "android-arm64")
    set(OPENSSL_TARGET_Android_i686       "android-x86")
    set(OPENSSL_TARGET_Android_x86_64     "android-x86_64")
    set(OPENSSL_TARGET_iOS_OS64           "ios64-xcrun")
    set(OPENSSL_TARGET_iOS_SIMULATOR64    "darwin64-x86_64-cc")
    set(OPENSSL_TARGET_iOS_SIMULATORARM64 "iossimulator-xcrun")
    set(OPENSSL_TARGET_Windows_           "mingw64")

    set(TARGET_KEY_Android ${CMAKE_SYSTEM_PROCESSOR})
    set(TARGET_KEY_iOS     ${PLATFORM})
    set(TARGET_KEY         ${TARGET_KEY_${CMAKE_SYSTEM_NAME}})

    set(OPENSSL_TARGET ${OPENSSL_TARGET_${CMAKE_SYSTEM_NAME}_${TARGET_KEY}})

    if(NOT DEFINED OPENSSL_TARGET)
        message(FATAL_ERROR "Unsupported OS x target combination: ${CMAKE_SYSTEM_NAME} x ${TARGET_KEY}")
    endif()

    set(BUILT_OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/include)
    set(BUILT_OPENSSL_SSL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(BUILT_OPENSSL_CRYPTO_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX})

    # XXX: Why are these so different per platform?
    # XXX: the -no-* options might actually be ignored as the documentation doesn't list them
    #      with the leading dash and says every misspelled option will be ignored
    #      https://wiki.openssl.org/index.php/Compilation_and_Installation
    set(OPENSSL_CONFIGURE_FLAGS_Android no-shared -no-ssl2 -no-ssl3 -no-comp -no-hw -no-engine)
    set(OPENSSL_CONFIGURE_FLAGS_iOS     no-shared -no-dso -no-hw -no-engine -fembed-bitcode)
    set(OPENSSL_CONFIGURE_FLAGS_Windows -no-shared -no-ssl3 -no-comp -no-engine)
    set(OPENSSL_CONFIGURE_FLAGS ${OPENSSL_CONFIGURE_FLAGS_${CMAKE_SYSTEM_NAME}})
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    externalproject_add(built_openssl
        URL ${OPENSSL_URL}
        URL_HASH ${OPENSSL_URL_HASH}
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && ./Configure
                ${OPENSSL_TARGET}
                ${OPENSSL_CONFIGURE_FLAGS}
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/openssl/install
                # `-U` removes the NDK built in definition to avoid redefinition warnings
                # https://github.com/openssl/openssl/issues/18561
                -U__ANDROID_API__
                # By default OpenSSL will use the highest available Android API
                # but we it to use the one we use in the rest of the code.
                # https://github.com/openssl/openssl/blob/master/NOTES-ANDROID.md
                -D__ANDROID_API__=${ANDROID_PLATFORM_LEVEL}
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
 elseif (${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    externalproject_add(built_openssl
        URL ${OPENSSL_URL}
        URL_HASH ${OPENSSL_URL_HASH}
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && export ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}
            && export PATH=${COMPILER_DIR}:$ENV{PATH}
            && ./Configure
                ${OPENSSL_TARGET}
                ${OPENSSL_CONFIGURE_FLAGS}
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
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    externalproject_add(built_openssl
        URL ${OPENSSL_URL}
        URL_HASH ${OPENSSL_URL_HASH}
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/openssl"
        CONFIGURE_COMMAND
            cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && set PATH=${COMPILER_DIR};$ENV{PATH}
            && export CC=${CMAKE_C_COMPILER}
            && ./Configure
            ${OPENSSL_TARGET}
                ${OPENSSL_CONFIGURE_FLAGS}
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/openssl/install
                --libdir=${CMAKE_CURRENT_BINARY_DIR}/openssl/install/lib
        ${BUILD_JOB_SERVER_AWARE}
        BUILD_COMMAND
            cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
            && set PATH=${COMPILER_DIR};$ENV{PATH}
            && make depend
            && make build_libs
        BUILD_BYPRODUCTS
            ${BUILT_OPENSSL_SSL_LIBRARY}
            ${BUILT_OPENSSL_CRYPTO_LIBRARY}
        ${INSTALL_JOB_SERVER_AWARE}
        INSTALL_COMMAND
        cd ${CMAKE_CURRENT_BINARY_DIR}/openssl/src/built_openssl
        && set PATH=${COMPILER_DIR};$ENV{PATH}
        && make install_dev
    )
endif()

if (DEFINED OPENSSL_VERSION)
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
