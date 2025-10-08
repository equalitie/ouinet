
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

    if (OUISYNC_SRC_DIR)
        # Use this to debug with local Ouisync sources
        add_subdirectory("${OUISYNC_SRC_DIR}/bindings/cpp" "ouisync" EXCLUDE_FROM_ALL)
    else()
        # Otherwise download from Git
        # Inspired by https://crascit.com/2015/07/25/cmake-gtest/

        # Download
        configure_file(
            cmake/dependencies/ouisync-download.txt.in
            ouisync/download/CMakeLists.txt
        )
        # Configure
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/ouisync/download"
        )
        # Build
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build .
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/ouisync/download"
        )
        # Import targets
        add_subdirectory("${CMAKE_BINARY_DIR}/ouisync/src/bindings/cpp")
    endif()
else()
    set(CPP_OUISYNC_LIBRARIES)
    set(OUISERVICE_OUISYNC_CPP_FILES)
    if (OUISYNC_SRC_DIR)
        message(FATAL_ERROR "OUISYNC_SRC_DIR is set to '${OUISYNC_SRC_DIR}' but WITH_OUISYNC is '${WITH_OUISYNC}'")
    endif()
endif()

