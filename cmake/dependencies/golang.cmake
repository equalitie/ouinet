include(ExternalProject)

set(GOROOT "${CMAKE_BINARY_DIR}/golang")

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin"
    OR "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    set(GO_VERSION "1.20.5.darwin-arm64")
    set(GO_MD5SUM "5d9b81ef1df799c5ed3a6e7c4eb30307")
else()
    set(GO_VERSION "1.12.3.linux-amd64")
    set(GO_MD5SUM "eac797050ce084d444a49e8d68ad13b7")
endif()

externalproject_add(golang
    URL https://dl.google.com/go/go${GO_VERSION}.tar.gz
    URL_MD5 ${GO_M5DSUM}
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
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin"
        OR "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    set(GOOS "darwin")
    set(GO_CC ${CMAKE_C_COMPILER})
    set(GO_CXX ${CMAKE_CXX_COMPILER})
    set(CMAKE_SYSTEM_PROCESSOR "arm64")
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
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
    set(GOARCH "arm64")
else()
    message(FATAL_ERROR "unsupported system processor ${CMAKE_SYSTEM_PROCESSOR}")
endif()

