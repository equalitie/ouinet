cmake_minimum_required (VERSION 3.5)
################################################################################

# NOTE: from_iso_extended_string has been introduced in Boost 1.62
set(BOOST_VERSION 1.62)
include(ExternalProject)

################################################################################
project(liblampshade)

set(CMAKE_CXX_STANDARD 14)

find_package(Threads)
find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS
    coroutine
    context
    thread
    system)

# Convert system name into GOOS.
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    set(GOOS "linux")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(GOOS "android")
else()
    message(FATAL_ERROR "unsupported system name ${CMAKE_SYSTEM_NAME}")
endif()

# Convert system processor into GOARCH (and maybe GOARM).
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    set(GOARCH "amd64")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(GOARCH "arm64")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a")
    set(GOARCH "arm")
    set(GOARM "7")
elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^arm.*")
    set(GOARCH "arm")
    set(GOARM "6")
else()
    message(FATAL_ERROR "unsupported system processor ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Build target tag with the Android API version if relevant.
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(TARGET "${CMAKE_SYSTEM_NAME}${CMAKE_SYSTEM_VERSION}--${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(TARGET "${CMAKE_SYSTEM_NAME}--${CMAKE_SYSTEM_PROCESSOR}")
endif()

################################################################################
add_definitions(-DBOOST_COROUTINE_NO_DEPRECATION_WARNING
                -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

################################################################################
# TODO(peterj): Address sanitizer seems to cause segmentation faults on exit
#               from main(?).
#set(SANITIZE "-fsanitize=address")
################################################################################

if(NOT TARGET golang)
    set(GOROOT "${CMAKE_CURRENT_BINARY_DIR}/golang")
    externalproject_add(golang
        URL https://dl.google.com/go/go1.11.2.linux-amd64.tar.gz
        URL_MD5 5630231012b6d02b821af51f04c2776c
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        SOURCE_DIR ${GOROOT}
    )
endif()

################################################################################

set(GOPATH "${CMAKE_CURRENT_BINARY_DIR}/go-workspace")

externalproject_add(lampshade
    GIT_REPOSITORY https://github.com/getlantern/lampshade.git
    GIT_TAG bca9c93b5723449e98c5077b75fc8b31387339c6
    DEPENDS golang
    CONFIGURE_COMMAND ""
    BUILD_COMMAND mkdir -p ${GOPATH}/src/github.com/getlantern
               && rsync -r ${CMAKE_CURRENT_BINARY_DIR}/lampshade-prefix/src/lampshade ${GOPATH}/src/github.com/getlantern
               && export PATH=${GOROOT}/bin:$ENV{PATH}
               && export GOROOT=${GOROOT}
               && export GOPATH=${GOPATH}
               && go get -v -d github.com/getlantern/lampshade
    INSTALL_COMMAND ""
)

set(BINDING_DIR "${CMAKE_CURRENT_BINARY_DIR}/lampshade_bindings")
set(BINDINGS_HEADER  "${BINDING_DIR}/lampshade_bindings.h")
set(BINDINGS_LIBRARY "${BINDING_DIR}/liblampshade_bindings.so")
set(BINDINGS_OUTPUT ${BINDINGS_HEADER} ${BINDINGS_LIBRARY})

add_custom_command(
    OUTPUT ${BINDINGS_OUTPUT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lampshade_bindings/lampshade_bindings.go
            golang
            lampshade
    COMMAND mkdir -p ${BINDING_DIR}
         && export PATH=${GOROOT}/bin:$ENV{PATH}
         && export GOROOT=${GOROOT}
         && export GOPATH=${GOPATH}
         && export GOOS=${GOOS}
         && export GOARCH=${GOARCH}
         && export GOARM=${GOARM}
         && export CGO_ENABLED=1
         && export CC=${CMAKE_C_COMPILER}
         && mkdir -p ${GOPATH}/src/github.com/equalitie/lampshade/src
         && cd ${GOPATH}/src/github.com/equalitie/lampshade
         && rsync -r ${CMAKE_CURRENT_SOURCE_DIR}/ src
         && go build -ldflags '-extldflags -Wl,-soname,liblampshade_bindings.so'
                     --buildmode=c-shared
                     -o ${BINDING_DIR}/lampshade_bindings.so
                     ./src/lampshade_bindings
         && chmod u+w -R ${GOPATH} # Modules cache is ro by default which CI doesn't cope with; https://github.com/golang/go/issues/27161
         && mv ${BINDING_DIR}/lampshade_bindings.so ${BINDING_DIR}/liblampshade_bindings.so
)

add_custom_target(lampshade-bindings-target DEPENDS ${BINDINGS_OUTPUT})
add_library(lampshade-bindings SHARED IMPORTED GLOBAL)
add_dependencies(lampshade-bindings lampshade-bindings-target)
set_target_properties(lampshade-bindings PROPERTIES IMPORTED_LOCATION ${BINDINGS_LIBRARY})

################################################################################
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BINDING_DIR}"
)

file(GLOB sources
    "./*.cpp"
)

add_library(lampshade-asio STATIC ${sources})
set_target_properties(lampshade-asio PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(lampshade-asio lampshade-bindings ${Boost_LIBRARIES} boost_asio)

set(LIBLAMPSHADE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)
set(LIBLAMPSHADE_TARGET "lampshade-asio" PARENT_SCOPE)
set(LIBLAMPSHADE_LIBRARIES
    lampshade-asio
    lampshade-bindings
    boost_asio
    PARENT_SCOPE
)
