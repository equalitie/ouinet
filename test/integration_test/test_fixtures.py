# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Constants used in the test
import logging


class TestFixtures:
    LOGGING_LEVEL = logging.DEBUG  # Change true to turn on debugging
    KEEP_IO_ALIVE_PULSE_INTERVAL = 60  # seconds

    FATAL_ERROR_INDICATOR_REGEX = r"[\s\S]*\[ABORT\][\s\S]*"
    DEFAULT_PROCESS_TIMEOUT = 15  # seconds
    TCP_TRANSPORT_TIMEOUT = 30
    I2P_TRANSPORT_TIMEOUT = 600
    IPFS_CACHE_TIMEOUT = 900
    BEP44_CACHE_TIMEOUT = 900

    TEST_TIMEOUT = {
        "test_i2p_transport": I2P_TRANSPORT_TIMEOUT,
        "test_tcp_transport": TCP_TRANSPORT_TIMEOUT,
        "test_ipfs_cache": IPFS_CACHE_TIMEOUT,
        "test_bep44_cache": BEP44_CACHE_TIMEOUT,
        "test_bep44_seed": BEP44_CACHE_TIMEOUT,
        "test_tcp_cache": TCP_TRANSPORT_TIMEOUT,
        "test_cache": TCP_TRANSPORT_TIMEOUT,
        "test_wikipedia_mainline": BEP44_CACHE_TIMEOUT,
    }

    # BENCHMARK REGEX INDICES
    READY_REGEX_INDEX = 0
    INDEX_READY_REGEX_INDEX = 1
    REQUEST_CACHED_REGEX_INDEX = 2

    REPO_FOLDER_NAME = "repos"

    INJECTOR_CONF_FILE_NAME = "ouinet-injector.conf"
    INJECTOR_CONF_FILE_CONTENT = "open-file-limit = 32768\n"

    I2P_INJECTOR_NAME = "i2p_injector"
    I2P_TUNNEL_READY_REGEX = r"[\s\S]*I2P Tunnel has been established"

    I2P_CLIENT = {"name": "i2p_client", "port": 8076}

    MAX_NO_OF_I2P_CLIENTS = 5
    MAX_NO_OF_TRIAL_I2P_REQUESTS = 5

    TCP_INJECTOR_NAME = "tcp_injector"
    TCP_INJECTOR_PORT_READY_REGEX = r"[\s\S]*TCP address[\s\S]*"
    TCP_INJECTOR_PORT = 7070

    CACHE_INJECTOR_NAME = "cache_injector"

    TEST_PAGE_BODY = b"<html><body>TESTPAGE</body></html>\n"
    TEST_HTTP_SERVER_PORT = 7080
    RESPONSE_LENGTH = 20

    CLIENT_CONFIG_FILE_NAME = "ouinet-client.conf"
    TCP_CLIENT = {"name": "tcp_client", "port": 8075, "fe_port": 8099}
    TCP_CLIENT_PORT_READY_REGEX = r"[\s\S]*listening to browser requests[\s\S]*"
    TCP_CLIENT_DISCOVERY_START = (
        r"[\s\S]*LocalPeerDiscovery: starting with advertised endpoints[\s\S]*"
    )
    CACHE_CLIENT = [
        {"name": "cache_client_1", "port": 8074, "fe_port": 8078},
        {"name": "cache_client_2", "port": 8073, "fe_port": 8079},
    ]
    CACHE_CLIENT_REQUEST_STORED_REGEX = "[\s\S]*HTTP store:[\s\S]*"
    CACHE_CLIENT_UTP_REQUEST_SERVED = "[\s\S]*serve_utp_req/serve_local END[\s\S]*"
    FIRST_CLIENT_CONF_FILE_CONTENT = "open-file-limit = 4096\n"

    IPNS_ID_ANNOUNCE_REGEX = "[\s\S]*IPNS Index: ([A-Za-z0-9]+)[\s\S]*"
    BEP44_PUBK_ANNOUNCE_REGEX = "[\s\S]*BEP44 Index: ([0-9A-Fa-f]+)[\s\S]*"
    # BEP5_PUBK_ANNOUNCE_REGEX = "HTTP signing public key \(Ed25519\): ([a-zA-Z0-9]+)"
    BEP5_PUBK_ANNOUNCE_REGEX = (
        "[\s\S]*HTTP signing public key \(Ed25519\): ([a-zA-Z0-9]+)[\s\S]*"
    )
    BEP5_REQUEST_CACHED_REGEX = "[\s\S]*X-Ouinet-Injection:[\s\S]*"
    START_OF_IPNS_RESOLUTION_REGEX = r"[\s\S]*Resolving IPNS address: [\s\S]*"
    IPFS_CACHE_READY_REGEX = r"[\s\S]*IPNS ID has been resolved successfully[\s\S]*"
    BEP44_CACHE_READY_REGEX = r"[\s\S]*BEP44 index: bootstrapped BitTorrent DHT[\s\S]*"
    IPFS_REQUEST_CACHED_REGEX = (
        r"[\s\S]*Request was successfully published to cache[\s\S]*"
    )
    BEP44_REQUEST_CACHED_REGEX = r"[\s\S]*BEP44 index: inserted key[\s\S]*"
    BEP44_RESPONSE_CACHED_REGEX = r"[\s\S]*BEP44 index: insertion finished[\s\S]*"
    NO_OF_CACHED_MESSAGES_REQUIRED = 1
    RETRIEVED_FROM_CACHE_REGEX = r"[\s\S]*Response was retrieved from cache[\s\S]*"
    MAX_NO_OF_TRIAL_CACHE_REQUESTS = 5
    TRIAL_CACHE_REQUESTS_WAIT = 20

    MAINNET_INJECTOR_HASH = "zh6ylt6dghu6swhhje2j66icmjnonv53tstxxvj6acu64sc62fnq"
    FRESH_SUCCESS_REGEX = r'[\s\S]*fresh/injector Finish; ec="Success"[\s\S]*'
    DHT_CONTACTS_STORED_REGEX = r"[\s\S]*DHT: Successfully stored contacts[\s\S]*"

    I2P_DHT_ADVERTIZE_WAIT_PERIOD = 30
    I2P_TUNNEL_HEALING_PERIOD = 10
    INJECTOR_CERT_PATH = ""  # will be filled during setup
    INJECTOR_CERTIFICATE = """
-----BEGIN CERTIFICATE-----
MIICyTCCAbGgAwIBAgIGAWwvE3jIMA0GCSqGSIb3DQEBCwUAMBQxEjAQBgNVBAMM
CWxvY2FsaG9zdDAeFw0xOTA3MjQxNjE4MjFaFw0zNDA3MjIxNjE4MjFaMBQxEjAQ
BgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB
AOQ6tX1fh1JQJGMEpgEaqFdVpl2Jz39s+3pFJAHRQMxvQa1a4pGwlc4smrhh8Y2Z
Kli8zhIzFPATZ3ipdBwnLBBUnDqpZWEqsKdBGGJghM+8EitXJwtSWjR2qqZcz3Xz
60MKt2S2IeL6L3/HtHM1bN93Xo3hQK/WYDQ6BEeLd6JSsns1mwwccTStu/kc3Y2E
IXPh1otQ624QXb9szIdwQw7vzi0saXONdaFFbpRyoa6KKCEC7iHHfUbEhCSRpL8Y
Mrl5z9mKqA8y+5tl3jzTHRtYE4SVG60pmd9nMQ33ue8m5ADq5Bd8Jg2qOmmg0KNF
V1RHB3pljMGco6eP9zmb3jsCAwEAAaMhMB8wHQYDVR0OBBYEFMCGT2KEmo4kM08C
E/rv/BbnmVfnMA0GCSqGSIb3DQEBCwUAA4IBAQBWxR7x1vADpkpVRzNxicLgd0CY
itmhEWtRQp9kE33O5BjRlHQ5TTA0WBp8Nc3c5ZZ1qAnQx3tXVZ7W1QY2XjiQpsPE
hFcPsAtFLP+kpDEFPi39iFv4gunR4M1zReCDTGTJ48bLtqONZ9XgJ7obW8r+TjuJ
yI/i11NWUwKldg0NevF1Bkddbhpt7PJHUpSSbwr3GJOKHfRw9ZaX6P86MVcJd0Ta
AzZPXqk+2eab43GbbD6keXRGIufMThKGyrRX+9aIaV3tx3uWAOfWVmlzf9w3gV3D
lmjPSOXmUsOLk0PFwoy7O7n9zJKNrUy1N2O+j0tH5HVXOnSjpS8aNrMtpfHS
-----END CERTIFICATE-----
"""
