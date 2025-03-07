# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Constants used in the test
import logging

class TestFixtures:
    LOGGING_LEVEL = logging.DEBUG #Change true to turn on debugging 
    KEEP_IO_ALIVE_PULSE_INTERVAL = 60 #seconds

    SIMULATE_I2P_EXTERNAL_DISCOVERY = True
    
    FATAL_ERROR_INDICATOR_REGEX = r'[\s\S]*\[ABORT\][\s\S]*'
    DEFAULT_PROCESS_TIMEOUT = 15 # seconds
    TCP_TRANSPORT_TIMEOUT = 30
    I2P_TRANSPORT_TIMEOUT = 600
    IPFS_CACHE_TIMEOUT = 900
    BEP44_CACHE_TIMEOUT = 900
    
    TEST_TIMEOUT = {
        "test_externally_discovered_i2p_injector": I2P_TRANSPORT_TIMEOUT,
        "test_i2p_i2cp_server": I2P_TRANSPORT_TIMEOUT,
        "test_i2p_transport": I2P_TRANSPORT_TIMEOUT,
        "test_tcp_transport": TCP_TRANSPORT_TIMEOUT,
        "test_ipfs_cache": IPFS_CACHE_TIMEOUT,
        "test_bep44_cache": BEP44_CACHE_TIMEOUT,
        "test_bep44_seed": BEP44_CACHE_TIMEOUT}

    #BENCHMARK REGEX INDICES
    READY_REGEX_INDEX = 0
    INDEX_READY_REGEX_INDEX = 1
    REQUEST_CACHED_REGEX_INDEX = 2
    
    
    REPO_FOLDER_NAME = "repos"

    INJECTOR_CONF_FILE_NAME = "ouinet-injector.conf"
    INJECTOR_CONF_FILE_CONTENT = "open-file-limit = 32768\n"

    I2P_INJECTOR_NAME = "i2p_injector"
    I2P_TUNNEL_READY_REGEX = r'[\s\S]*I2P Tunnel has been established'
    I2P_TUNNELLER_LISTENING_REGEX = r'[\s\S]*I2P tunneller listening[\s\S]*'
    I2P_INJECTOR_ADDRESS_RECEIVED_REGEX = r'[\s\S]*Received: I2P seeder[\s\S]*'
    I2P_CLIENT_FINISHED_READING_REGEX = r'[\s\S]*Finish reading[\s\S]*'
    I2P_CLIENT_ERROR_READING_REGEX = r'[\s\S]*Error in getting i2p seeder response[\s\S]*'

    I2P_CLIENT = {"name":"i2p_client", "port": 3888, "i2cp_port": 7654,}
    I2P_DISCOVERED_ID_ANNOUNCE_PORT = 8998
    
    MAX_NO_OF_I2P_CLIENTS = 5
    MAX_NO_OF_TRIAL_I2P_REQUESTS = 5
    
    TCP_INJECTOR_NAME = "tcp_injector"
    TCP_PORT_READY_REGEX = r'[\s\S]*Successfully listening on TCP Port[\s\S]*'
    TCP_INJECTOR_PORT = 7070

    CACHE_INJECTOR_NAME = "cache_injector"
    
    TEST_PAGE_BODY=b"<html><body>TESTPAGE</body></html>\n"
    TEST_HTTP_SERVER_PORT = 7080
    RESPONSE_LENGTH = 20

    CLIENT_CONFIG_FILE_NAME = "ouinet-client.conf"

    TCP_CLIENT = { "name": "tcp_client",
                         "port": 8075}

    CACHE_CLIENT = [{ "name": "cache_client_1",
                         "port": 8074},
                      { "name": "cache_client_2",
                         "port": 8073}]

    FIRST_CLIENT_CONF_FILE_CONTENT = "open-file-limit = 4096\n"

    IPNS_ID_ANNOUNCE_REGEX = "[\s\S]*IPNS Index: ([A-Za-z0-9]+)[\s\S]*"
    BEP44_PUBK_ANNOUNCE_REGEX = "[\s\S]*BEP44 Index: ([0-9A-Fa-f]+)[\s\S]*"
    START_OF_IPNS_RESOLUTION_REGEX = r'[\s\S]*Resolving IPNS address: [\s\S]*'
    IPFS_CACHE_READY_REGEX = r'[\s\S]*IPNS ID has been resolved successfully[\s\S]*'
    BEP44_CACHE_READY_REGEX = r'[\s\S]*BEP44 index: bootstrapped BitTorrent DHT[\s\S]*'
    IPFS_REQUEST_CACHED_REGEX = r'[\s\S]*Request was successfully published to cache[\s\S]*'
    BEP44_REQUEST_CACHED_REGEX = r'[\s\S]*BEP44 index: inserted key[\s\S]*'
    BEP44_RESPONSE_CACHED_REGEX = r'[\s\S]*BEP44 index: insertion finished[\s\S]*'
    NO_OF_CACHED_MESSAGES_REQUIRED = 1
    RETRIEVED_FROM_CACHE_REGEX = r'[\s\S]*Response was retrieved from cache[\s\S]*'
    MAX_NO_OF_TRIAL_CACHE_REQUESTS = 3
    TRIAL_CACHE_REQUESTS_WAIT = 20

    I2P_DHT_ADVERTIZE_WAIT_PERIOD = 30
    I2P_TUNNEL_HEALING_PERIOD = 10
    #TEST_PAGE_BODY=b"<html><body>" + b" "*2**20 + b"</body></html>\n"
