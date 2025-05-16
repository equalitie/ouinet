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

if(WIN32)
    add_compile_options("-O1")
    add_definitions(-DBOOST_USE_WINDOWS_H -DWIN32_LEAN_AND_MEAN)
endif()

################################################################################
project(ouinet)

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
    INTERFACE -std=c++20
)
target_include_directories(ouinet_dependencies
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(ouinet_dependencies
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
add_library(lib::asio_utp ALIAS asio_utp)

###########################################################
set(CPPUPnP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/cpp-upnp)
find_package(CPPUPnP REQUIRED)
target_compile_definitions(cpp_upnp PUBLIC -DBOOST_ASIO_SEPARATE_COMPILATION)
###########################################################

if(WITH_EXPERIMENTAL)
    target_compile_definitions(ouinet_dependencies
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
corrosion_set_env_vars(ouinet_rs
    "CXXFLAGS=-I${CMAKE_CURRENT_BINARY_DIR}/boost/install/include -I${CMAKE_CURRENT_SOURCE_DIR}/rust")
add_dependencies(cargo-prebuild_ouinet_rs built_boost)

# This is not necessry to build the ouinet_rs library, but using it
# automatically adds the include directories to every target that has ouinet_rs
# as dependency.
target_include_directories(ouinet_rs
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/rust
    # XXX: Isn't there a better way to obtain the cargo target directory?
    ${CMAKE_BINARY_DIR}/cargo/build/${Rust_CARGO_TARGET_CACHED}/cxxbridge)

################################################################################

file(GLOB ouinet_common_sources
    "./src/logger.cpp"
    "./src/connect_to_host.cpp"
    "./src/response_part.cpp"
    "./src/util.cpp"
    "./src/util/atomic_file.cpp"
    "./src/util/random.cpp"
    "./src/util/handler_tracker.cpp"
    "./src/util/crypto.cpp"
    "./src/util/file_io.cpp"
    "./src/util/temp_file.cpp"
    "./src/util/hash.cpp"
    "./src/ssl/ca_certificate.cpp"
    "./src/bittorrent/*.cpp"
    "./src/bep5_swarms.cpp"
    "./src/ouiservice.cpp"
    "./src/ouiservice/tcp.cpp"
    "./src/ouiservice/utp.cpp"
    "./src/ouiservice/tls.cpp"
    "./src/ouiservice/multi_utp_server.cpp"
    "./src/util/persistent_lru_cache.cpp"
    "./src/endpoint.cpp"
    "./src/async_sleep.cpp"
    "./src/http_util.cpp"
    ${EXPERIMENTAL_SOURCES}
)

add_library(ouinet_common STATIC ${ouinet_common_sources})

target_link_libraries(ouinet_common
    PRIVATE
        ouinet_dependencies
        lib::uri
        OpenSSL::Crypto
        ${EXPERIMENTAL_LIBRARIES}
    PUBLIC
        Boost::iostreams
        lib::gcrypt
        lib::json
        lib::asio_utp
        ouinet_rs
        # Boost::asio_ssl is public for Android because the ClientConfig used
        # in native-lib.cpp contains ssl::context and we need its move
        # constructor and destructor implementations.
        Boost::asio_ssl
)

target_include_directories(ouinet_common PUBLIC "src")

################################################################################
file(GLOB client_sources
    "./src/client.cpp"
    "./src/client_config.cpp"
    "./src/client_front_end.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp"
    "./src/ssl/dummy_certificate.cpp"
    "./src/ouiservice/bep5/client.cpp"
    "./src/ouiservice/connect_proxy.cpp"
    "./src/cache/*.cpp"
    "./src/util/atomic_dir.cpp"
    "./src/util/temp_dir.cpp"
    "./src/util/reachability.cpp"
    "./src/doh.cpp"
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
    PUBLIC
        ouinet_common
        cpp_upnp
    PRIVATE
        lib::zdnsparser
)

################################################################################
if (WITH_INJECTOR)
    file(GLOB injector_sources
        "./src/injector.cpp"
        "./src/ouiservice/bep5/server.cpp"
        "./src/endpoint.cpp"
        "./src/cache/http_sign.cpp"
        "./src/http_logger.cpp"
        ${EXPERIMENTAL_SOURCES}
    )

    add_executable(injector ${injector_sources})
    target_link_libraries(injector ouinet_common)

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
