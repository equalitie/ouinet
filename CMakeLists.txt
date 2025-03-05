if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # CMAKE_ANDROID_STANDALONE_TOOLCHAIN (and likely other cross compilation
    # android variables) was only introduced in v3.7
    cmake_minimum_required (VERSION 3.7)
else()
    cmake_minimum_required (VERSION 3.5)
endif()

################################################################################
# Configurable options
option(WITH_INJECTOR "Compile injector" ON)

# NOTE: Need to disable ASan until Boost.Asio switches to Boost.Coroutine2
#       because trowing (and catching) exceptions inside a coroutine confuses
#       ASan. More info:
#       https://github.com/boostorg/coroutine/issues/30#issuecomment-325574878
option(WITH_ASAN "Build with Address sanitizer" OFF)

option(WITH_EXPERIMENTAL "Build with experimental features" OFF)



################################################################################
# Global build flags
if (WITH_ASAN)
    add_compile_options("-fsanitize=address" "-fno-omit-frame-pointer")
endif()
if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    add_compile_options("-ggdb")
endif()
add_compile_options("-Wall")


################################################################################
project(ouinet)


set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH})
include(OuinetDependencies)
include(BuildVersion)


# This target needs to be linked by all code that includes ouinet headers.
add_library(ouinet_base INTERFACE)
add_library(ouinet::base ALIAS ouinet_base)
target_compile_options(ouinet_base
    INTERFACE -std=c++17
)
target_include_directories(ouinet_base
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(ouinet_base
    INTERFACE
        Boost::asio
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
if (WITH_ASAN)
    # NOTE: https://stackoverflow.com/a/50163972
    target_link_libraries(ouinet_base
        INTERFACE -fsanitize=address
    )
endif()

###########################################################
option(ASIO_UTP_WITH_TESTS             "" OFF)
option(ASIO_UTP_WITH_EXAMPLES          "" OFF)
option(ASIO_UTP_DEBUG_LOGGING          "" OFF)
option(UTP_DEBUG_LOGGING               "" OFF)
add_subdirectory(./modules/asio-utp)
add_library(lib::asio_utp ALIAS asio_utp)

###########################################################
set(CPPUPnP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/cpp-upnp)
find_package(CPPUPnP REQUIRED)
###########################################################

if(WITH_EXPERIMENTAL)
    target_compile_definitions(ouinet_base
            INTERFACE -D__EXPERIMENTAL__
    )
    add_subdirectory(./src/ouiservice/i2p)
    #add_subdirectory(./src/ouiservice/lampshade)
    add_subdirectory(./src/ouiservice/obfs4proxy)
    set(EXPERIMENTAL_SOURCES
            #    "./src/ouiservice/lampshade.cpp"
            "./src/ouiservice/pt-obfs2.cpp"
            "./src/ouiservice/pt-obfs3.cpp"
            "./src/ouiservice/pt-obfs4.cpp"
            "./src/ouiservice/pluggable-transports/*.cpp"
    )
    set(EXPERIMENTAL_LIBRARIES
            #    ouinet::lampshade-asio
            ouinet::ouiservice-i2p
    )
endif()

################################################################################
# ouinet-rs (ouinet components written in rust)

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml)
corrosion_set_env_vars(ouinet_rs CXXFLAGS=-I${CMAKE_CURRENT_BINARY_DIR}/boost/install/include)
add_dependencies(cargo-prebuild_ouinet_rs built_boost)

set(OUINET_RS_INCLUDE_DIR
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rust
    # XXX: Isn't there a better way to obtain the cargo target directory?
    ${CMAKE_BINARY_DIR}/cargo/build/${Rust_CARGO_TARGET_CACHED}/cxxbridge)

################################################################################
file(GLOB client_sources
    "./src/client.cpp"
    "./src/connect_to_host.cpp"
    "./src/client_front_end.cpp"
    "./src/endpoint.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp"
    "./src/ouiservice.cpp"
    "./src/response_part.cpp"
    "./src/bep5_swarms.cpp"
    "./src/ssl/ca_certificate.cpp"
    "./src/ssl/dummy_certificate.cpp"
    "./src/ouiservice/tcp.cpp"
    "./src/ouiservice/utp.cpp"
    "./src/ouiservice/tls.cpp"
    "./src/ouiservice/bep5/client.cpp"
    "./src/ouiservice/multi_utp_server.cpp"
    "./src/ouiservice/connect_proxy.cpp"
    "./src/logger.cpp"
    "./src/cache/*.cpp"
    "./src/bittorrent/*.cpp"
    "./src/util/atomic_dir.cpp"
    "./src/util/atomic_file.cpp"
    "./src/util/persistent_lru_cache.cpp"
    "./src/util/crypto.cpp"
    "./src/util/random.cpp"
    "./src/util/file_io.cpp"
    "./src/util/temp_dir.cpp"
    "./src/util/temp_file.cpp"
    "./src/util/hash.cpp"
    "./src/util/reachability.cpp"
    "./src/util.cpp"
    "./src/util/handler_tracker.cpp"
    "./src/doh.cpp"
    "./src/http_util.cpp"
    ${EXPERIMENTAL_SOURCES}
    ${VERSION_CPP}
)

if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_executable(client ${client_sources})
else()
    add_library(client SHARED ${client_sources})
    add_library(ouinet::client ALIAS client)
    find_library(android_log log)
    target_link_libraries(client PUBLIC ${android_log})
endif()

target_link_libraries(client
    PRIVATE
        Boost::asio_ssl
        Boost::iostreams
        lib::asio_utp
        cpp_upnp
        lib::zdnsparser
        lib::gcrypt
        lib::json
        lib::uri
        OpenSSL::Crypto
        ${EXPERIMENTAL_LIBRARIES}
        ouinet_rs
    PUBLIC
        ouinet::base
)
target_include_directories(client ${OUINET_RS_INCLUDE_DIR})

################################################################################
if (WITH_INJECTOR)
    file(GLOB injector_sources
        "./src/injector.cpp"
        "./src/connect_to_host.cpp"
        "./src/ouiservice.cpp"
        "./src/response_part.cpp"
        "./src/bep5_swarms.cpp"
        "./src/ouiservice/tcp.cpp"
        "./src/ouiservice/utp.cpp"
        "./src/ouiservice/tls.cpp"
        "./src/ouiservice/bep5/server.cpp"
        "./src/ouiservice/multi_utp_server.cpp"
        "./src/ssl/ca_certificate.cpp"
        "./src/logger.cpp"
        "./src/endpoint.cpp"
        "./src/cache/http_sign.cpp"
        "./src/bittorrent/*.cpp"
        "./src/util/atomic_file.cpp"
        "./src/util/persistent_lru_cache.cpp"
        "./src/util/crypto.cpp"
        "./src/util/random.cpp"
        "./src/util/file_io.cpp"
        "./src/util/temp_file.cpp"
        "./src/util/hash.cpp"
        "./src/util/handler_tracker.cpp"
        "./src/util.cpp"
        "./src/http_util.cpp"
        ${EXPERIMENTAL_SOURCES}
    )

    add_executable(injector ${injector_sources})

    target_link_libraries(injector
        Boost::asio_ssl
        Boost::iostreams
        lib::asio_utp
        lib::gcrypt
        lib::json
        lib::uri
        OpenSSL::Crypto
        ouinet::base
        ouinet_rs
        ${EXPERIMENTAL_LIBRARIES}
    )

    target_include_directories(injector ${OUINET_RS_INCLUDE_DIR})

endif() # if WITH_INJECTOR


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
if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_subdirectory(test)
endif()
################################################################################
