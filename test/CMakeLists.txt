
set(ASIO_UTP_INCLUDE_DIR "../modules/asio-utp/include")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "../src")

######################################################################
add_executable(test-parser "test_parser.cpp")

######################################################################
add_executable(test-cache "test_cache_control.cpp"
                          "../src/cache_control.cpp"
                          "../src/http_util.cpp"
                          "../src/logger.cpp")

add_dependencies(test-cache uri)
target_link_libraries(test-cache ${Boost_LIBRARIES} ${URI_LIBRARY} boost_asio)

######################################################################
add_executable(test-wait-condition "test_wait_condition.cpp")
target_link_libraries(test-wait-condition ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-scheduler "test_scheduler.cpp")
target_include_directories(test-scheduler PUBLIC "${Boost_INCLUDE_DIR}")
target_link_libraries(test-scheduler ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-timeout-stream "test_timeout_stream.cpp")

target_link_libraries(test-timeout-stream ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-stream-fork "test-stream-fork.cpp")

target_link_libraries(test-stream-fork ${Boost_LIBRARIES} boost_asio)

################################################################################
add_executable(test-persistent-lru-cache "test_persistent_lru_cache.cpp"
                                         "../src/util/persistent_lru_cache.cpp"
                                         "../src/util/hash.cpp"
                                         "../src/util/atomic_file.cpp"
                                         "../src/util/crypto.cpp"
                                         "../src/util/file_io.cpp"
                                         "../src/util/temp_file.cpp")

target_include_directories(test-persistent-lru-cache PUBLIC "${Boost_INCLUDE_DIR}")
add_dependencies(test-persistent-lru-cache gcrypt)
target_link_libraries(test-persistent-lru-cache ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} boost_asio)

################################################################################
file(GLOB bt_cpp_files "../src/bittorrent/*.cpp"
                       "../src/util/crypto.cpp"
                       "../src/util/hash.cpp"
                       "../src/logger.cpp")

add_executable(test-bittorrent "test_bittorrent.cpp" ${bt_cpp_files})

target_include_directories(test-bittorrent PUBLIC
    "${ASIO_UTP_INCLUDE_DIR}"
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-bittorrent
    asio_utp ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} boost_asio)

add_dependencies(test-bittorrent gcrypt)

################################################################################
add_executable(test-dht "test-dht.cpp" "../src/util.cpp" ${bt_cpp_files})

target_include_directories(test-dht PUBLIC
    "${ASIO_UTP_INCLUDE_DIR}"
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-dht
    asio_utp ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} boost_asio z)

add_dependencies(test-dht gcrypt)

################################################################################
add_executable(
    test-routing-table
    "test-routing-table.cpp"
    "../src/util.cpp" ${bt_cpp_files})

target_include_directories(test-routing-table PUBLIC
    "${ASIO_UTP_INCLUDE_DIR}"
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-routing-table
    asio_utp ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} boost_asio z)

add_dependencies(test-routing-table gcrypt)

################################################################################
add_executable(test-bep5 "test-bep5.cpp" ${bt_cpp_files})

target_include_directories(test-bep5 PUBLIC
    "${ASIO_UTP_INCLUDE_DIR}"
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-bep5 asio_utp ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} boost_asio)
add_dependencies(test-bep5 gcrypt)

######################################################################
add_executable(test-watch-dog "test_watch_dog.cpp")
target_link_libraries(test-watch-dog ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-logger "test_logger.cpp"
                           "../src/logger.cpp")
                           
target_link_libraries(test-logger ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-connection-pool "test-connection-pool.cpp")

target_link_libraries(test-connection-pool ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-oui-server "ouiservice-server.cpp"
                               "../src/logger.cpp"
                               "../src/endpoint.cpp"
                               "../src/ouiservice.cpp"
                               "../src/ouiservice/tcp.cpp"
)

target_link_libraries(test-oui-server ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-oui-client "ouiservice-client.cpp"
                               "../src/logger.cpp"
                               "../src/endpoint.cpp"
                               "../src/ouiservice.cpp"
                               "../src/ouiservice/tcp.cpp"
)

target_link_libraries(test-oui-client ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-http-util "test_http_util.cpp")
target_include_directories(test-http-util PUBLIC "${Boost_INCLUDE_DIR}")
target_link_libraries(test-http-util ${Boost_LIBRARIES} boost_asio)

######################################################################
add_executable(test-http-sign "test_http_sign.cpp"
                              "../src/cache/http_sign.cpp"
                              "../src/util.cpp"
                              "../src/util/crypto.cpp"
                              "../src/util/hash.cpp"
)
target_include_directories(test-http-sign PUBLIC "${Boost_INCLUDE_DIR}")
target_link_libraries(test-http-sign ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES} z boost_asio)
add_dependencies(test-http-sign gcrypt)
