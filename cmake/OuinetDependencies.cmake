# For Boost ASIO SSL and the I2P ouiservice.
# Must precede boost for asio_ssl.cpp
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/openssl.cmake)

# This is used all over Ouinet's source.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/boost.cmake)

# For crypto, hash and random utilities.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/gcrypt.cmake)

if(WITH_EXPERIMENTAL)
    # For Pluggable Transport modules (obfs4 and lampshade).
    include(${CMAKE_CURRENT_LIST_DIR}/dependencies/golang.cmake)
endif()

# For client front-end status API.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/json.cmake)

# For URL parsing and encoding/decoding.
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/url.cmake)

# Ouinet code written in rust
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/ouinet-rs.cmake)
