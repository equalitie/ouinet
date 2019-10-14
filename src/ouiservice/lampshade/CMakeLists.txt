cmake_minimum_required (VERSION 3.5)

include(ExternalProject)

project(liblampshade)

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

if (CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(LAMPSHADE_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
else()
    set(LAMPSHADE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

set(BINDINGS_DIR "${CMAKE_CURRENT_BINARY_DIR}/lampshade_bindings")
set(BINDINGS_LIBRARY
    ${LAMPSHADE_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}lampshade_bindings${CMAKE_SHARED_LIBRARY_SUFFIX}
)

add_custom_command(
    OUTPUT ${BINDINGS_LIBRARY}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lampshade_bindings/lampshade_bindings.go
            golang
            lampshade
    COMMAND mkdir -p ${BINDINGS_DIR}
         && export PATH=${GOROOT}/bin:$ENV{PATH}
         && export GOROOT=${GOROOT}
         && export GOPATH=${GOPATH}
         && export GOOS=${GOOS}
         && export GOARCH=${GOARCH}
         && export GOARM=${GOARM}
         && export CGO_ENABLED=1
         && export CC=${GO_CC}
         && mkdir -p ${GOPATH}/src/github.com/equalitie/lampshade/src
         && cd ${GOPATH}/src/github.com/equalitie/lampshade
         && rsync -r ${CMAKE_CURRENT_SOURCE_DIR}/ src
         && go build -ldflags '-extldflags -Wl,-soname,liblampshade_bindings.so'
                     --buildmode=c-shared
                     -o ${BINDINGS_DIR}/lampshade_bindings.so
                     ./src/lampshade_bindings
         && chmod u+w -R ${GOPATH} # Modules cache is ro by default which CI doesn't cope with; https://github.com/golang/go/issues/27161
         && cp ${BINDINGS_DIR}/lampshade_bindings.so ${BINDINGS_LIBRARY}
)
add_custom_target(lampshade-bindings DEPENDS ${BINDINGS_OUTPUT})



add_library(lampshade-asio STATIC "liblampshade.cpp")
add_library(ouinet::lampshade-asio ALIAS lampshade-asio)
set_target_properties(lampshade-asio PROPERTIES LINKER_LANGUAGE CXX)
add_dependencies(lampshade-asio lampshade-bindings)
target_include_directories(lampshade-asio
    PRIVATE ${BINDINGS_DIR}
)
target_link_libraries(lampshade-asio
    PRIVATE ${BINDINGS_LIBRARY}
    PUBLIC ouinet::base
)
