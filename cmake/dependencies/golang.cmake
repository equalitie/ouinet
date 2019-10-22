include(ExternalProject)

set(GOROOT "${CMAKE_BINARY_DIR}/golang")
externalproject_add(golang
    URL https://dl.google.com/go/go1.12.3.linux-amd64.tar.gz
    URL_MD5 eac797050ce084d444a49e8d68ad13b7
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    SOURCE_DIR ${GOROOT}
)

# Convert system name into GOOS.
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    set(GOOS "linux")
    set(GO_CC ${CMAKE_C_COMPILER})
    set(GO_CXX ${CMAKE_CXX_COMPILER})
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(GOOS "android")

    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    get_filename_component(COMPILER_TOOLCHAIN_PREFIX ${_CMAKE_TOOLCHAIN_PREFIX} NAME)
    string(REGEX REPLACE "-$" "" COMPILER_HOSTTRIPLE ${COMPILER_TOOLCHAIN_PREFIX})
    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a")
        set(COMPILER_HOSTTRIPLE "armv7a-linux-androideabi")
    endif()
    set(GO_CC ${COMPILER_DIR}/${COMPILER_HOSTTRIPLE}${ANDROID_PLATFORM_LEVEL}-clang)
    set(GO_CXX ${COMPILER_DIR}/${COMPILER_HOSTTRIPLE}${ANDROID_PLATFORM_LEVEL}-clang++)
else()
    message(FATAL_ERROR "unsupported system name ${CMAKE_SYSTEM_NAME}")
endif()

# Convert system processor into GOARCH (and maybe GOARM).
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    set(GOARCH "amd64")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
    set(GOARCH "386")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(GOARCH "arm64")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a")
    set(GOARCH "arm")
    set(GOARM "7")
else()
    message(FATAL_ERROR "unsupported system processor ${CMAKE_SYSTEM_PROCESSOR}")
endif()

