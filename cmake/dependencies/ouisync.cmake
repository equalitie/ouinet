if (WITH_OUISYNC)
    set(CPP_OUISYNC_LIBRARIES
        cpp_ouisync_client
        cpp_ouisync_service
    )
    set(OUISERVICE_OUISYNC_CPP_FILES
        "./src/ouiservice/ouisync/ouisync.cpp"
        "./src/ouiservice/ouisync/file.cpp"
        "./src/ouiservice/ouisync/error.cpp"
    )
    # For use in Ouinet code
    add_compile_definitions(WITH_OUISYNC)

    # Tell Ouisync cmake file we're using a separately Boost.Asio library
    set(OUISYNC_BOOST_ASIO_SEPARATE_COMPILATION ON CACHE BOOL "" FORCE)

    # Enable/Disable mounting (Virtual File System)
    set(OUISYNC_WITH_VFS OFF CACHE BOOL "" FORCE)

    if (OUISYNC_SRC_DIR)
        # Use this to debug with local Ouisync sources
        set(OUISYNC_CPP_SRC_DIR "${OUISYNC_SRC_DIR}/bindings/cpp")
    else()
        # Otherwise download from Git
        FetchContent_Declare(
            ouisync
            GIT_REPOSITORY "https://github.com/equalitie/ouisync"
            GIT_TAG        "392385b040277123bb315165e20c43176a9ad8c0"
        )
        FetchContent_MakeAvailable(ouisync)

        FetchContent_GetProperties(ouisync)
        set(OUISYNC_CPP_SRC_DIR "${ouisync_SOURCE_DIR}/bindings/cpp")
    endif()

    # Import targets
    add_subdirectory("${OUISYNC_CPP_SRC_DIR}" "ouisync/output" EXCLUDE_FROM_ALL)

    target_link_libraries(cpp_ouisync_service PRIVATE ouinet_asio)
    target_link_libraries(cpp_ouisync_client PRIVATE ouinet_asio)
else()
    set(CPP_OUISYNC_LIBRARIES)
    set(OUISERVICE_OUISYNC_CPP_FILES)
endif()
