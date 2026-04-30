# For Boost ASIO SSL and the I2P ouiservice.
# Must precede boost for asio_ssl.cpp
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/openssl.cmake)

# This is used all over Ouinet's source.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/boost.cmake)

if(WITH_DEPRECATED)
    # For Pluggable Transport modules (obfs4 and lampshade).
    include(${CMAKE_CURRENT_LIST_DIR}/dependencies/golang.cmake)
endif()

# For client front-end status API.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/json.cmake)

# Hardcoded CA certificates
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/ca-certs.cmake)
