cmake_minimum_required (VERSION 3.5)

include(ExternalProject)

set(GOPATH "${CMAKE_CURRENT_BINARY_DIR}/go-workspace")

if (CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(OBFS4PROXY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
else()
    set(OBFS4PROXY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

#
# Using CGO_ENABLED causes go to use the external linker shipped by the
# android ndk. This is almost certainly the wrong way to do it, but it
# seems to work for now.
#
externalproject_add(obfs4proxy
    URL https://github.com/Yawning/obfs4/archive/obfs4proxy-0.0.9.tar.gz
    URL_MD5 5ec7e4d96bf57fa0b269083076aacd02
    DEPENDS golang
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 1
    BUILD_COMMAND mkdir -p ${GOPATH}
               && export PATH=${GOROOT}/bin:$ENV{PATH}
               && export GOROOT=${GOROOT}
               && export GOPATH=${GOPATH}
               && export GOOS=${GOOS}
               && export GOARCH=${GOARCH}
               && export GOARM=${GOARM}
               && export CC=${GO_CC}
               && export CGO_ENABLED=1
	       && export GIT_SSL_NO_VERIFY=1 # XXX: go get gitlab.com/yawning/utls.git@v0.0.9-2 fails on GL CI otherwise
               && go build -o ${OBFS4PROXY_OUTPUT_DIRECTORY}/obfs4proxy ./obfs4proxy
               && chmod u+w -R ${GOPATH} # Modules cache is ro by default which CI doesn't cope with; https://github.com/golang/go/issues/27161
    INSTALL_COMMAND ""
)
