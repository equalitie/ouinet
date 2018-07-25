# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Constants used in the test
import logging

class TestFixtures:
    LOGGING_LEVEL = logging.DEBUG #Change true to turn on debugging 
    
    FATAL_ERROR_INDICATOR_REGEX = r'[\s\S]*\[ABORT\][\s\S]*'
    DEFAULT_PROCESS_TIMEOUT = 15 # seconds
    TCP_TRANSPORT_TIMEOUT = 15 
    I2P_TRANSPORT_TIMEOUT = 300
    IPFS_CACHE_TIMEOUT = 600
    
    PROCESS_DYING_SLACK = 30

    TEST_TIMEOUT = {
        "test_i2p_transport":300,
        "test_tcp_transport":15,
        "test_ipfs_cache":1200}

    #BENCHMARK REGEX INDICES
    READY_REGEX_INDEX = 0
    REQUEST_CACHED_REGEX_INDEX = 1
    
    
    REPO_FOLDER_NAME = "repos"

    INJECTOR_CONF_FILE_NAME = "ouinet-injector.conf"
    INJECTOR_CONF_FILE_CONTENT = "open-file-limit = 32768\n"

    INJECTOR_I2P_PUBLIC_ID = "tR0ADsik-0Gn4F-t6WICzUT59H9NlUnejCknd4E4XZNha8B0zR8zFW6va~MpCzMdlE7FxChpHM9MjMnuR1PcakaKU9i5E0wJ~cP2oQd1GTCtMARmpsILysN4brfEvU2TAn1n7JZnHpMHSRPaPTqk7g-ReLR8jN5yQBOedxmmKIJspcOhTjRprqceKjoPpmHzqPgVtrspgQAOIUCcRQ3S44DGUfN603Woqlci6XDmMZW4gktHEygCoIbaXMbvt7gCY6S5PI5ENu-3xsKKZG5B7RUzDQGX5iQHfRLe9utGMRQf63RneFuXZ6hfMSSv7TXm7emUpw5gDXFoLK9GT3NDPPmjX3kU-SlNxF2BfI38YU8fuqguZEjQO0w89O8DbyKvqxBTS5scxucIw5Gu3qZX98If20QzVqk2ZaEeA8LTCIWsM4mi9Mmw~fD7fyX9fnmGNFHrDkXbNtZU-8K9TzB4Ka4KHJRBQIlNDKCT3LMKs5PJuW4TYtNb2M8UV2UsIFouBQAEAAEAAA=="

    INJECTOR_I2P_PRIVATE_KEY = "tR0ADsik-0Gn4F-t6WICzUT59H9NlUnejCknd4E4XZNha8B0zR8zFW6va~MpCzMdlE7FxChpHM9MjMnuR1PcakaKU9i5E0wJ~cP2oQd1GTCtMARmpsILysN4brfEvU2TAn1n7JZnHpMHSRPaPTqk7g-ReLR8jN5yQBOedxmmKIJspcOhTjRprqceKjoPpmHzqPgVtrspgQAOIUCcRQ3S44DGUfN603Woqlci6XDmMZW4gktHEygCoIbaXMbvt7gCY6S5PI5ENu-3xsKKZG5B7RUzDQGX5iQHfRLe9utGMRQf63RneFuXZ6hfMSSv7TXm7emUpw5gDXFoLK9GT3NDPPmjX3kU-SlNxF2BfI38YU8fuqguZEjQO0w89O8DbyKvqxBTS5scxucIw5Gu3qZX98If20QzVqk2ZaEeA8LTCIWsM4mi9Mmw~fD7fyX9fnmGNFHrDkXbNtZU-8K9TzB4Ka4KHJRBQIlNDKCT3LMKs5PJuW4TYtNb2M8UV2UsIFouBQAEAAEAAE7cmUpeDXDkMEy3Br08viGl5MDBvVe9DGRiGyg-WzIm8uQ0054Q2APSmljEDN21JFMnrvbYn1BgCzCOXKJjd4T9C7Ms~NR0XJGSUMiIiclKHBYNkP~SoFiBX2PD3CThfq56t7HikE9EtO1K9rLEh5uwoxqyyBdbqxEN6yzDZOX5znajTr~6rwZ0EebaNBG9cb76UAA69gc-9HfpYE0py~yb6qcfQuXoQy0m8gVMffJymNw-sXbNJMzb8qHAeH8i24LRleLU-MrvqmAw7RM1Tg3AlK4e3d0I3acgnGsPmqdir01F5Sbb3ETpmIU6sfngbs9pGwZ7ZKio2r1hzlZX7EkvaLJhUhGZJWwzxqHDhhX6llDF168OsJHlOoeqPU35Ug=="

    I2P_INJECTOR_NAME = "i2p_injector"
    I2P_TUNNEL_READY_REGEX = r'[\s\S]*I2P Tunnel has been established'

    I2P_CLIENT = {"name":"i2p_client",
                        "port": 8081}

    MAX_NO_OF_TRIAL_I2P_REQUESTS = 5
    
    TCP_INJECTOR_NAME = "tcp_injector"
    TCP_PORT_READY_REGEX = r'[\s\S]*Successfully listening on TCP Port[\s\S]*'
    TCP_INJECTOR_PORT = 7070

    CACHE_INJECTOR_NAME = "cache_injector"
    
    TEST_PAGE_BODY="<html><body>TESTPAGE</body></html>\n"
    TEST_HTTP_SERVER_PORT = 7080
    RESPONSE_LENGTH = 20

    CLIENT_CONFIG_FILE_NAME = "ouinet-client.conf"

    TCP_CLIENT = { "name": "tcp_client",
                         "port": 8081}

    CACHE_CLIENT = [{ "name": "cache_client_1",
                         "port": 8084},
                      { "name": "cache_client_2",
                         "port": 8085}]

    FIRST_CLIENT_CONF_FILE_CONTENT = "open-file-limit = 4096\n"

    IPNS_ID_ANNOUNCE_REGEX = "[\s\S]*IPNS DB: ([A-Za-z0-9]+)\n[\s\S]*"
    IPFS_CACHE_READY_REGEX = r'[\s\S]*IPNS has been resolved successfully[\s\S]*'
    REQUEST_CACHED_REGEX = r'[\s\S]*Request was successfully published to cache[\s\S]*'
    NO_OF_CACHED_MESSAGES_REQUIRED = 2 
    SERVED_FROM_CACHE_REGEX = r'[\s\S]*Response was served from cache[\s\S]*'

