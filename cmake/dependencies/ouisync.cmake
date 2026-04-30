# Tell Ouisync cmake file we're using a separately Boost.Asio library
set(OUISYNC_BOOST_ASIO_SEPARATE_COMPILATION ON CACHE BOOL "" FORCE)

# Enable/Disable mounting (Virtual File System)
set(OUISYNC_WITH_VFS OFF CACHE BOOL "" FORCE)

if (OUISYNC_SRC_DIR)
    # Use this to debug with local Ouisync sources
    add_subdirectory(
        "${OUISYNC_SRC_DIR}/bindings/cpp"
        "${CMAKE_BINARY_DIR}/ouisync"
        EXCLUDE_FROM_ALL
    )
else()
    # Otherwise download from Git
    FetchContent_Declare(
        ouisync
        GIT_REPOSITORY "https://github.com/equalitie/ouisync"
        GIT_TAG        "392385b040277123bb315165e20c43176a9ad8c0"
    )
    FetchContent_MakeAvailable(ouisync)

    FetchContent_GetProperties(ouisync)
    add_subdirectory("${ouisync_SOURCE_DIR}/bindings/cpp" EXCLUDE_FROM_ALL)
endif()

target_link_libraries(cpp_ouisync_service PRIVATE ouinet_asio)
target_link_libraries(cpp_ouisync_client  PRIVATE ouinet_asio)

if (WITH_OUISYNC)
    add_library(
        ouinet_ouisync
        EXCLUDE_FROM_ALL
        "./src/ouiservice/ouisync/ouisync.cpp"
        "./src/ouiservice/ouisync/error.cpp"
    )
    target_link_libraries(ouinet_ouisync
        PUBLIC
            cpp_ouisync_client
        PRIVATE
            cpp_ouisync_service
            ouinet_common
    )
    target_include_directories(ouinet_ouisync PUBLIC "./src")
    add_compile_definitions(WITH_OUISYNC)
else()
    add_library(ouinet_ouisync INTERFACE EXCLUDE_FROM_ALL)
    target_include_directories(ouinet_ouisync INTERFACE "./src")
endif()
