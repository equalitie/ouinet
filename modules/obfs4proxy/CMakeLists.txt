cmake_minimum_required (VERSION 3.5)

include(ExternalProject)

################################################################################
project(obfs4proxy)

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

set(GOPATH "${CMAKE_CURRENT_BINARY_DIR}/go-workspace")

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
               && export CC=${CMAKE_C_COMPILER}
               && export CGO_ENABLED=1
	       && export GIT_SSL_NO_VERIFY=1 # XXX: go get gitlab.com/yawning/utls.git@v0.0.9-2 fails on GL CI otherwise
               && go build -o ${CMAKE_CURRENT_BINARY_DIR}/obfs4proxy ./obfs4proxy
               && chmod u+w -R ${GOPATH} # Modules cache is ro by default which CI doesn't cope with; https://github.com/golang/go/issues/27161
    INSTALL_COMMAND ""
)
