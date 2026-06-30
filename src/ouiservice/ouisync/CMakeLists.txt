if (WITH_OUISYNC)
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
        include(FetchContent)

        FetchContent_Declare(
            ouisync
            GIT_REPOSITORY "https://github.com/equalitie/ouisync"
            GIT_TAG        "869004e84fb35e2f850f7b4e0f912979f6d72ed2"
        )

        FetchContent_MakeAvailable(ouisync)

        FetchContent_GetProperties(ouisync)

        add_subdirectory(
            "${ouisync_SOURCE_DIR}/bindings/cpp"
            "${CMAKE_BINARY_DIR}/ouisync"
            EXCLUDE_FROM_ALL
        )
    endif()

    target_link_libraries(cpp_ouisync_service PRIVATE ouinet_asio)
    target_link_libraries(cpp_ouisync_client  PRIVATE ouinet_asio)

    add_library(
        ouinet_ouisync
        EXCLUDE_FROM_ALL
        OBJECT
        "./error.cpp"
        "./ouisync.cpp"
        "./socket.cpp"
    )
    ouinet_setup(TARGET ouinet_ouisync
        LINK PUBLIC ouinet_rs cpp_ouisync_service cpp_ouisync_client
        DEFS OUINET_API_OUISYNC_EXPORT
    )

    set_target_properties(cpp_ouisync_service PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    set_target_properties(cpp_ouisync_client PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
else()
    add_library(ouinet_ouisync INTERFACE EXCLUDE_FROM_ALL)
    target_include_directories(ouinet_ouisync INTERFACE "${CMAKE_SOURCE_DIR}/src")
endif()
