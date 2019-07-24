# Must precede boost for asio_ssl.cpp
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/openssl.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/dependencies/boost.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/gcrypt.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/golang.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/json.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/dependencies/uri.cmake)
