cmake_minimum_required (VERSION 3.13)

# Set cmake policies to avoid warnings in newer versions of cmake
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.19)
    cmake_policy(SET CMP0114 NEW)
endif()

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

################################################################################
project(ouinet)

################################################################################
# Configurable options

# Configurable options
option(WITH_TEST "Compile tests" ON)

# NOTE: Need to disable ASan until Boost.Asio switches to Boost.Coroutine2
#       because trowing (and catching) exceptions inside a coroutine confuses
#       ASan. More info:
#       https://github.com/boostorg/coroutine/issues/30#issuecomment-325574878
option(WITH_ASAN "Build with Address sanitizer" OFF)

option(WITH_EXPERIMENTAL "Build with experimental features" OFF)
option(WITH_OUISYNC "Build with Ouisync" OFF)
option(OUINET_HARDCODE_CA_CERTS "Hardcode CA certificates" OFF)
option(OUINET_MEASURE_BUILD_TIMES "Show compilation and link times" OFF)

option(WITH_DEPRECATED "Build with deprecated features" OFF)

################################################################################
# show summary

message(STATUS "---------------------------------------")
message(STATUS "Options:")
message(STATUS "  WITH_TEST                : ${WITH_TEST}")
message(STATUS "  WITH_ASAN                : ${WITH_ASAN}")
message(STATUS "  WITH_EXPERIMENTAL        : ${WITH_EXPERIMENTAL}")
message(STATUS "  WITH_OUISYNC             : ${WITH_OUISYNC}")
message(STATUS "  WITH_DEPRECATED          : ${WITH_DEPRECATED}")
message(STATUS "  OUINET_HARDCODE_CA_CERTS : ${OUINET_HARDCODE_CA_CERTS}")
message(STATUS "")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ compiler ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ compiler version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "---------------------------------------")

################################################################################
# Global build flags
if (WITH_ASAN)
    add_compile_options("-fsanitize=address" "-fno-omit-frame-pointer")
    link_libraries(-fsanitize=address)
endif()
if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    add_compile_options("-ggdb")
endif()

add_compile_options("-Wall")

if(WIN32 OR MINGW)
    add_compile_options("-O1")
    add_definitions(-DBOOST_USE_WINDOWS_H -DWIN32_LEAN_AND_MEAN)
endif()

if(APPLE AND NOT IOS)
    # Boost.Stacktrace doesn't need _GNU_SOURCE on macOS
    add_definitions(-DBOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED)
endif()

if ((NOT ANDROID) AND (NOT IOS))
    # Adds -fPIC when building static libraries (which I think is the default
    # when building dynamic ones). It's needed on x86 when linking the
    # `client_lib` target.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (OUINET_MEASURE_BUILD_TIMES)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/measure_time.sh")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "${CMAKE_CURRENT_SOURCE_DIR}/cmake/measure_time.sh")
endif()

################################################################################

if(MINGW) # Note that MING gets defined only after `project(ouinet)`
    # Fixes errors
    #   "Fatal error: can't write 103 bytes to section .text of <CPP_FILE>.obj: 'file too big'"
    # https://digitalkarabela.com/mingw-w64-how-to-fix-file-too-big-too-many-sections/
    add_compile_options(-Wa,-mbig-obj)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH})
include(OuinetDependencies)
include(BuildVersion)

# This target needs to be linked by all code that includes ouinet headers.
add_library(ouinet_dependencies INTERFACE)
target_compile_options(ouinet_dependencies
    INTERFACE -std=c++23
)
target_include_directories(ouinet_dependencies
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(ouinet_dependencies
    INTERFACE
        ouinet_asio
        Boost::boost
        Boost::date_time
        Boost::filesystem
        Boost::nowide
        Boost::program_options
        Boost::regex
        Boost::system
        # generic_stream.h needs openssl headers, but not the library.
        OpenSSL::headers
)
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows" AND BOOST_VERSION GREATER_EQUAL 1.72.0)
    # explicitly link with bcrypt after Boost::filesystem
    target_link_libraries(ouinet_dependencies
        INTERFACE
          bcrypt
    )
endif()
if (WITH_ASAN)
    # NOTE: https://stackoverflow.com/a/50163972
    target_link_libraries(ouinet_dependencies
        INTERFACE -fsanitize=address
    )
endif()

###########################################################
option(ASIO_UTP_WITH_TESTS             "" OFF)
option(ASIO_UTP_WITH_EXAMPLES          "" OFF)
option(ASIO_UTP_DEBUG_LOGGING          "" OFF)
option(UTP_DEBUG_LOGGING               "" OFF)
add_subdirectory(./modules/asio-utp)

###########################################################
set(CPPUPnP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/cpp-upnp)
find_package(CPPUPnP REQUIRED)
target_compile_definitions(cpp_upnp PUBLIC -DBOOST_ASIO_SEPARATE_COMPILATION)
###########################################################

add_subdirectory(./src/ouiservice/i2p)
add_subdirectory(./rust)

###########################################################

if(WITH_DEPRECATED)
    #add_subdirectory(./src/ouiservice/lampshade)
    add_subdirectory(./src/ouiservice/obfs4proxy)
    set(DEPRECATED_SOURCES
            "./src/ouiservice/lampshade.cpp"
            "./src/ouiservice/pt-obfs2.cpp"
            "./src/ouiservice/pt-obfs3.cpp"
            "./src/ouiservice/pt-obfs4.cpp"
            "./src/ouiservice/pluggable-transports/*.cpp"
    )
    set(DEPRECATED_LIBRARIES
            ouinet::lampshade-asio
    )
endif()
################################################################################
# These are separate from `ouinet_common` because they're also required by
# `ouinet_rs`
add_library(ouinet_common_public
    STATIC
    "./src/logger.cpp"
    "./src/util/log_path.cpp"
)
target_link_libraries(ouinet_common_public PUBLIC Boost::filesystem)

################################################################################

file(GLOB ouinet_common_sources
    "./src/connect_to_host.cpp"
    "./src/response_part.cpp"
    "./src/util.cpp"
    "./src/util/atomic_file.cpp"
    "./src/util/random.cpp"
    "./src/util/handler_tracker.cpp"
    "./src/util/sign.cpp"
    "./src/util/file_io.cpp"
    "./src/util/temp_file.cpp"
    "./src/util/scrypt.cpp"
    "./src/util/url.cpp"
    "./src/util/yield.cpp"
    "./src/cache/http_sign.cpp"
    "./src/cache/signed_head.cpp"
    "./src/ssl/ca_certificate.cpp"
    "./src/ssl/util.cpp"
    "./src/bittorrent/*.cpp"
    "./src/bep5_swarms.cpp"
    "./src/ouiservice.cpp"
    "./src/ouiservice/tcp.cpp"
    "./src/ouiservice/utp.cpp"
    "./src/ouiservice/tls.cpp"
    "./src/ouiservice/multi_utp_server.cpp"
    "./src/util/persistent_lru_cache.cpp"
    "./src/endpoint.cpp"
    "./src/session.cpp"
    "./src/error.cpp"
    "./src/async_sleep.cpp"
    "./src/http_util.cpp"
    "./src/response_reader.cpp"
    ${DEPRECATED_SOURCES}
)

# TODO: Some low level openssl functions we use are deprecated.
set_source_files_properties("./src/ssl/ca_certificate.cpp" PROPERTIES COMPILE_FLAGS -Wno-deprecated-declarations)

add_library(ouinet_common STATIC ${ouinet_common_sources})

if (OUINET_HARDCODE_CA_CERTS)
    set(OUINET_HARDCODED_CA_CERTS hardcode_ca_certs)
endif()

target_link_libraries(ouinet_common
    PRIVATE
        Boost::url
        OpenSSL::Crypto
        ${DEPRECATED_LIBRARIES}
    PUBLIC
        ${OUINET_HARDCODED_CA_CERTS}
        ouinet::ouiservice-i2p
        ouinet_dependencies
        Boost::iostreams
        lib::json
        asio_utp
        ouinet_rs
        ouinet_common_public
        # Boost::asio_ssl is public for Android because the ClientConfig used
        # in native-lib.cpp contains ssl::context and we need its move
        # constructor and destructor implementations.
        ouinet_asio_ssl
)

target_include_directories(ouinet_common PUBLIC "src")

################################################################################
if(WITH_DEPRECATED)
    target_compile_definitions(ouinet_common
            INTERFACE -D__DEPRECATED__
    )
endif()
################################################################################

file(GLOB client_sources
    "./src/client.cpp"
    "./src/client_config.cpp"
    "./src/client_front_end.cpp"
    "./src/client_lib.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp"
    "./src/ssl/dummy_certificate.cpp"
    "./src/ouiservice/bep5/client.cpp"
    "./src/ouiservice/connect_proxy.cpp"
    "./src/cache/announcer.cpp"
    "./src/cache/client.cpp"
    "./src/cache/dht_groups.cpp"
    "./src/cache/hash_list.cpp"
    "./src/cache/http_store.cpp"
    "./src/cache/local_peer_discovery.cpp"
    "./src/cache/multi_peer_reader.cpp"
    "./src/cache/multi_peer_reader_error.cpp"
    "./src/cache/resource_id.cpp"
    "./src/cache/resource_key.cpp"
    "./src/util/atomic_dir.cpp"
    "./src/util/temp_dir.cpp"
    "./src/request.cpp"
    "./src/peer_message.cpp"
    ${VERSION_CPP}
)

if (IOS)
    add_library(client_lib STATIC ${client_sources})
else()
    add_library(client_lib SHARED ${client_sources})
endif()
if (NOT WIN32 AND NOT MINGW)
    set_target_properties(client_lib PROPERTIES OUTPUT_NAME "client")
endif()
add_library(ouinet::client ALIAS client_lib)


target_link_libraries(client_lib
    PUBLIC
        ouinet_common
        ouinet_ouisync
        cpp_upnp
)

if (WIN32 OR MINGW)
    target_compile_definitions(client_lib PRIVATE -DOUINET_DECL_EXPORT)
endif()

if (ANDROID)
    find_library(android_log log)
    target_link_libraries(client_lib PUBLIC ${android_log})
elseif (IOS)
    set(ouinet_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ios/ouinet)
    find_package(ouinet REQUIRED)
else()
    add_executable(client "./src/client_main.cpp")
    target_link_libraries(client client_lib)
endif()

################################################################################
file(GLOB injector_sources
    "./src/injector.cpp"
    "./src/injector_config.cpp"
    "./src/ouiservice/bep5/server.cpp"
    "./src/cache/http_sign.cpp"
    "./src/cache/signed_head.cpp"
    "./src/http_logger.cpp"
    ${DEPRECATED_SOURCES}
)

add_library(injector_lib SHARED ${injector_sources})
if (NOT WIN32 AND NOT MINGW)
    set_target_properties(injector_lib PROPERTIES OUTPUT_NAME "injector")
endif ()

target_link_libraries(injector_lib ouinet_common)

if (WIN32 OR MINGW)
    target_compile_definitions(injector_lib PRIVATE -DOUINET_DECL_EXPORT)
endif()


if ((NOT ANDROID) AND (NOT IOS))
    add_executable(injector "./src/injector_main.cpp")
    target_link_libraries(injector injector_lib)
endif()


################################################################################
# Config file directory

set(config_files
    "repos/client/ouinet-client.conf"
    "repos/injector/ouinet-injector.conf"
)
set(config_targets )

if(NOT ${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_BINARY_DIR})
    foreach(config_file ${config_files})
        get_filename_component(config_dir ${config_file} DIRECTORY)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${config_file}
            MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/${config_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/${config_dir}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/${config_file} ${CMAKE_CURRENT_BINARY_DIR}/${config_file}
        )
        set(config_targets ${config_targets} ${CMAKE_CURRENT_BINARY_DIR}/${config_file})
    endforeach()
    add_custom_target(configfiles ALL DEPENDS ${config_targets})
endif()

################################################################################
# Tests
if ((NOT ANDROID) AND WITH_TEST)
    add_subdirectory(test)
endif()
################################################################################
