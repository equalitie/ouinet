if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # CMAKE_ANDROID_STANDALONE_TOOLCHAIN (and likely other cross compilation
    # android variables) was only introduced in v3.7
    cmake_minimum_required (VERSION 3.7)
else()
    cmake_minimum_required (VERSION 3.5)
endif()

set(BOOST_VERSION 1.67)

include(ExternalProject)
################################################################################
# Configurable options
option(WITH_INJECTOR "Compile injector" ON)
option(WITH_ASAN "Build with Address sanitizer" OFF)

################################################################################
# NOTE: https://stackoverflow.com/questions/37603238/fsanitize-not-using-gold-linker-in-gcc-6-1
# NOTE2: Need to disable ASan until Boost.Asio switches to Boost.Coroutine2
#        because trowing (and catching) exceptions inside a coroutine confuses
#        ASan. More info:
#        https://github.com/boostorg/coroutine/issues/30#issuecomment-325574878
if (WITH_ASAN)
    set(SANITIZE "-fsanitize=address -fuse-ld=gold")
endif() # if WITH_ASAN

# For some reason we need to define both of these
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
add_definitions(-DBOOST_COROUTINE_NO_DEPRECATION_WARNING)

# Asio has some trouble when multiple libraries are using the header only
# version.  E.g. we've had problems that boost::asio::error returned from one
# library wasn't equal to the same error from another library (because they had
# different error categories).  The last problem before enabling this separate
# compilation was when Android was running the io_service::run function, but
# some handlers were not executed inside client.cpp (notably the
# acceptor::async_accept function never called it's callback).
#
# https://www.boost.org/doc/libs/1_67_0/doc/html/boost_asio/using.html#boost_asio.using.optional_separate_compilation
add_definitions(-DBOOST_ASIO_SEPARATE_COMPILATION)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # Boost.Beast has a patch that uses the ANDROID definition
    # instead of the implicit __ANDROID__ one.
    # https://github.com/boostorg/beast/blob/44f37d1a11d/include/boost/beast/core/impl/file_posix.ipp#L27
    add_definitions(-DANDROID)
endif()
################################################################################

set(GOROOT "${CMAKE_BINARY_DIR}/golang")
externalproject_add(golang
    URL https://dl.google.com/go/go1.11.2.linux-amd64.tar.gz
    URL_MD5 5630231012b6d02b821af51f04c2776c
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    SOURCE_DIR ${GOROOT}
)

################################################################################
option(ASIO_IPFS_WITH_EXAMPLE_BINARIES "" OFF)

add_subdirectory(./modules/asio-ipfs)
add_subdirectory(./modules/obfs4proxy)
add_subdirectory(./src/ouiservice/i2p)

################################################################################
project(ouinet)

find_package(OpenSSL REQUIRED)
find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS filesystem
                                                        regex
                                                        unit_test_framework
                                                        program_options
                                                        coroutine
                                                        context
                                                        thread
                                                        date_time
                                                        iostreams
                                                        system)
find_package(Threads)

################################################################################
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
################################################################################
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -Wall -O0 -ggdb ${SANITIZE}")
################################################################################
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(GCRYPT_HOST "--host=arm-unknown-linux-androideabi")
endif()

set(GPG_ERROR_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out)

externalproject_add(gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.32.tar.bz2
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND export CC=${CMAKE_C_COMPILER}
                   && ./configure ${GCRYPT_HOST} --prefix=${GPG_ERROR_PREFIX}
    BUILD_COMMAND $(MAKE)
    PREFIX gpg_error)

externalproject_add(gcrypt
    DEPENDS gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.8.3.tar.bz2
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
                      ./configure ${GCRYPT_HOST}
                        --with-libgpg-error-prefix=${GPG_ERROR_PREFIX}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    PREFIX gcrypt)

set(GCRYPT_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/gcrypt/src/gcrypt/src"
    "${GPG_ERROR_PREFIX}/include")

set(GCRYPT_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/gcrypt/src/gcrypt/src/.libs/libgcrypt.so")

################################################################################
externalproject_add(json
    # TODO: We only really need to download one header file.
    URL https://github.com/nlohmann/json/archive/v2.1.1.tar.gz
    BUILD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    PREFIX json)

set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/json/src/json/src")

# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses")

################################################################################
externalproject_add(uri
    GIT_REPOSITORY https://github.com/cpp-netlib/uri
    GIT_TAG 1.0.1
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -DUri_BUILD_TESTS=OFF
        -DUri_BUILD_DOCS=OFF
        -DUri_DISABLE_LIBCXX=""
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    PREFIX "uri")

set(URI_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/uri/src/uri/include")
set(URI_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/uri/src/uri-build/src/libnetwork-uri.a")

################################################################################
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${ASIO_IPFS_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}"
    "${JSON_INCLUDE_DIR}"
    "${URI_INCLUDE_DIR}"
)

file(GLOB client_sources
    "./src/client.cpp"
    "./src/asio.cpp"
    "./src/asio_ssl.cpp"
    "./src/connect_to_host.cpp"
    "./src/client_front_end.cpp"
    "./src/endpoint.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp"
    "./src/ouiservice.cpp"
    "./src/ssl/ca_certificate.cpp"
    "./src/ssl/dummy_certificate.cpp"
    "./src/ouiservice/pt-obfs2.cpp"
    "./src/ouiservice/pt-obfs3.cpp"
    "./src/ouiservice/pt-obfs4.cpp"
    "./src/ouiservice/tcp.cpp"
    "./src/ouiservice/tls.cpp"
    "./src/ouiservice/pluggable-transports/*.cpp"
    "./src/logger.cpp"
    "./src/cache/*.cpp"
    "./src/bittorrent/*.cpp"
    "./src/util/persistent_lru_cache.cpp"
    "./src/util/crypto.cpp"
    "./src/util/file_io.cpp"
    "./src/util/sha1.cpp"
    "./src/util.cpp"
    "./src/http_util.cpp"
)

if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_executable(client ${client_sources})
else()
    find_library( # Sets the name of the path variable.
                  log-lib
                  # Specifies the name of the NDK library that
                  # you want CMake to locate.
                  log )
    add_library(client SHARED ${client_sources})
endif()

target_link_libraries(client
    ${log-lib}
    ouiservice-i2p
    ${Boost_LIBRARIES}
    ${ASIO_IPFS_LIBRARIES}
    ${URI_LIBRARY}
    ${GCRYPT_LIBRARIES}
)

add_dependencies(client json uri ${ASIO_IPFS_TARGET} gcrypt)

################################################################################
if (WITH_INJECTOR)
    include_directories(
        "${Boost_INCLUDE_DIR}"
        "${ASIO_IPFS_INCLUDE_DIR}"
        "${GCRYPT_INCLUDE_DIR}"
    )

    file(GLOB injector_sources
        "./src/injector.cpp"
        "./src/asio.cpp"
        "./src/asio_ssl.cpp"
        "./src/connect_to_host.cpp"
        "./src/cache_control.cpp"
        "./src/ouiservice.cpp"
        "./src/ouiservice/pt-obfs2.cpp"
        "./src/ouiservice/pt-obfs3.cpp"
        "./src/ouiservice/pt-obfs4.cpp"
        "./src/ouiservice/tcp.cpp"
        "./src/ouiservice/tls.cpp"
        "./src/ouiservice/pluggable-transports/*.cpp"
        "./src/ssl/ca_certificate.cpp"
        "./src/logger.cpp"
        "./src/endpoint.cpp"
        "./src/cache/*.cpp"
        "./src/bittorrent/*.cpp"
        "./src/util/persistent_lru_cache.cpp"
        "./src/util/crypto.cpp"
        "./src/util/file_io.cpp"
        "./src/util/sha1.cpp"
        "./src/util.cpp"
        "./src/http_util.cpp"
    )

    add_executable(injector ${injector_sources})
    add_dependencies(injector json uri ${ASIO_IPFS_TARGET} gcrypt)

    target_link_libraries(injector
        ouiservice-i2p
        ${Boost_LIBRARIES}
        ${ASIO_IPFS_LIBRARIES}
        ${URI_LIBRARY}
        ${GCRYPT_LIBRARIES}
    )
endif() # if WITH_INJECTOR

################################################################################
# Config file directory

set(config_files
    "repos/client/ouinet-client.conf"
    "repos/injector/ouinet-injector.conf"
)
set(config_targets )

if(NOT ${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    add_custom_target(config ALL DEPENDS test)
    foreach(config_file ${config_files})
        get_filename_component(config_dir ${config_file} DIRECTORY)
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/${config_file}
            MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/${config_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/${config_dir}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/${config_file} ${CMAKE_BINARY_DIR}/${config_file}
        )
        set(config_targets ${config_targets} ${CMAKE_BINARY_DIR}/${config_file})
    endforeach()
    add_custom_target(appel ALL DEPENDS ${config_targets})
endif()

################################################################################
# Tests
if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_subdirectory(test)
endif()
################################################################################
