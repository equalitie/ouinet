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
        # Inspired by https://crascit.com/2015/07/25/cmake-gtest/

        file(WRITE ouisync/download/CMakeLists.txt [=[
            cmake_minimum_required(VERSION 2.8.2)
            project(ouisync-download NONE)
            
            include(ExternalProject)
            externalproject_add(ouisync
              GIT_REPOSITORY    https://github.com/equalitie/ouisync
              GIT_TAG           c2c4ff578f6728c068ba5a7577dce4f51f0aabea
              GIT_SHALLOW       true
              SOURCE_DIR        "${CMAKE_BINARY_DIR}/ouisync/src"
              BINARY_DIR        "${CMAKE_BINARY_DIR}/ouisync/build"
              # No building, that's done outside of this externalproject_add
              CONFIGURE_COMMAND ""
              BUILD_COMMAND     ""
              INSTALL_COMMAND   ""
              TEST_COMMAND      ""
            )
        ]=])

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

        set(OUISYNC_CPP_SRC_DIR "${CMAKE_BINARY_DIR}/ouisync/src/bindings/cpp")
    endif()

    # Import targets
    add_subdirectory("${OUISYNC_CPP_SRC_DIR}" "ouisync" EXCLUDE_FROM_ALL)
else()
    set(CPP_OUISYNC_LIBRARIES)
    set(OUISERVICE_OUISYNC_CPP_FILES)
endif()

