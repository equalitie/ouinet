# Copyright 2018, 2019 eQualit.ie, Inc.
# See LICENSE for other credits and copying information

# Integration tests for Ouinet - test for http communication offered through different transports and caches


from typing import List
import string

import sys
from os import rename
from os import remove
from os.path import exists
from shutil import rmtree
from time import time
from math import floor

import tempfile
from multiprocessing import Process
import asyncio

# Making random requests not to rely on cache
import random

import requests
import socket
from requests import Response as Reqresponse
from requests import ReadTimeout
from urllib.parse import urlparse

import logging
import pytest
import pytest_asyncio

from test_fixtures import TestFixtures
from test_http_server import spawn_http_server

from ouinet_process_controler import (
    OuinetConfig,
    OuinetClient,
    OuinetBEP5CacheInjector,
    OuinetProcess,
)


def safe_random_str(length):
    letters = string.ascii_lowercase
    return "".join(random.choice(letters) for i in range(length))


def get_nonloopback_ip() -> str:
    """Return a local IP which is not loopback.

    This is needed since injectors may block such requests for security
    reasons.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("192.0.2.1", 1))  # no actual traffic here
    ip = s.getsockname()[0]
    s.close()
    return ip


async def wait_for_injector_peer_candidates(frontend_port: int):
    tries = 0
    maxtries = 20
    while True:

        print(f"waiting for peers: try {tries+1}/{maxtries}")
        response = requests.get(f"http://localhost:{frontend_port}/api/status")
        response.raise_for_status()
        if response.json()["injector_ready"]:
            break
        peer_n = response.json()["injector_peers_n"]

        tries += 1
        if tries >= maxtries:
            raise Exception(
                f"No peers after {maxtries} tries, aborting. Number of peers reported:",
                peer_n,
            )
        await asyncio.sleep(3)


async def wait_for_benchmark(process: OuinetProcess, benchmark: str):
    start = time()
    while not process.callbacks[benchmark]:
        print("waiting for", benchmark, "-", floor(time() - start), "s", end="\r")
        await asyncio.sleep(1)
    print("successfully waited for", benchmark, "\n\n")


def run_tcp_injector(args):
    argv = args.copy()
    argv.append("--allow-private-targets")
    argv.extend(["--log-level", "SILLY"])

    # BEP5 is our default injector
    injector = OuinetBEP5CacheInjector(
        OuinetConfig(
            app_name=TestFixtures.TCP_INJECTOR_NAME + "_tcp",
            timeout=TestFixtures.TCP_TRANSPORT_TIMEOUT,
            argv=argv,
            # TODO: move it to the class itself
            benchmark_regexes=[
                TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX,
                TestFixtures.TCP_INJECTOR_PORT_READY_REGEX,
                TestFixtures.BEP5_REQUEST_CACHED_REGEX,
            ],
        ),
    )
    injector.start()
    proc_list.append(injector)

    return injector


def run_tcp_client(name, args):
    argv = args.copy()
    argv.append("--allow-private-targets")
    client = OuinetClient(
        OuinetConfig(
            name,
            TestFixtures.TCP_TRANSPORT_TIMEOUT,
            argv,
            benchmark_regexes=[
                TestFixtures.TCP_CLIENT_PORT_READY_REGEX,
                TestFixtures.TCP_CLIENT_DISCOVERY_START,
                TestFixtures.CACHE_CLIENT_REQUEST_STORED_REGEX,
                TestFixtures.CACHE_CLIENT_UTP_REQUEST_SERVED,
                TestFixtures.FRESH_SUCCESS_REGEX,
                TestFixtures.DHT_INITIALIZED_REGEX,
                TestFixtures.DHT_CONTACTS_STORED_REGEX,
                TestFixtures.RESPONSE_RECEIVED_FROM_CACHE,
            ],
        ),
    )
    client.start()
    proc_list.append(client)

    return client


def request_echo(proxy_port, echo_content) -> Reqresponse:
    """
    Send a get request to request the test server to echo the content
    """
    url = "http://%s:%d/?content=%s" % (
        get_nonloopback_ip(),
        TestFixtures.TEST_HTTP_SERVER_PORT,
        echo_content,
    )
    return request_url(proxy_port, url)


def request_url(port, url) -> Reqresponse:
    proxies = {"http": f"http://127.0.0.1:{port}"}
    host = urlparse(url).hostname
    headers = {"X-Ouinet-Group": host}
    print("sending request to", url)
    try:
        timeout = 10
        response = requests.get(url, proxies=proxies, headers=headers, timeout=timeout)
    except ReadTimeout:
        raise IOError(
            f"Client has took more than {timeout} seconds to respond. It is possible that the client is having a problem but does not report it via http codes."
        )

    return response


async def wait_for_dht_ready(client):
    print("[WARNING] Waiting for DHT to initialize, this can take 1-5 minutes")
    timestamp = time()
    await wait_for_benchmark(client, TestFixtures.DHT_INITIALIZED_REGEX)
    print(
        "[INFO] for DHT to get a WAN endpoint it took: ", time() - timestamp, "seconds"
    )


# Compatibility functions, not needed in newer tests
def assertTrue(value):
    assert value


def assertEquals(x, y):
    assert x == y


@pytest.fixture()
def http_server() -> Process:
    server = spawn_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

    yield server

    print("waiting for http server to terminate")
    server.terminate()
    server.join(timeout=2)
    print("http server terminated")


proc_list: List[OuinetProcess] = []


@pytest_asyncio.fixture(autouse=True)
async def process_janitor() -> List[OuinetProcess]:
    """
    This fixture is teardown-only, otherwise it forces too much async in code
    It is using repo janitor to make sure repos are cleaned after it and
    not before
    """
    yield
    for cur_proc in proc_list:
        print("stopping process", cur_proc)
        await cur_proc.stop()

    print("Done, no processes left")
    proc_list.clear()

    # Cleaning up process files
    # After processes quit so that they do not write anything
    repofolder = TestFixtures.REPO_FOLDER_NAME

    print("cleaning up the folder", repofolder)
    if exists(repofolder):
        rmtree(repofolder)
    assert not exists(repofolder)


@pytest.fixture()
def certificate_file() -> str:
    with tempfile.NamedTemporaryFile() as file:
        file.write(TestFixtures.INJECTOR_CERTIFICATE.encode("utf-8"))
        TestFixtures.INJECTOR_CERT_PATH = file.name + ".pem"
        file.flush()
        # it will be deleted otherwise but we do not want it to be deleted yet
        rename(file.name, TestFixtures.INJECTOR_CERT_PATH)

    yield TestFixtures.INJECTOR_CERT_PATH

    # Now we will delete it
    if exists(TestFixtures.INJECTOR_CERT_PATH):
        remove(TestFixtures.INJECTOR_CERT_PATH)


@pytest.fixture()
def log():
    logging.basicConfig(
        stream=sys.stdout,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
        level=TestFixtures.LOGGING_LEVEL,
    )


################# Tests #####################


@pytest.mark.timeout(TestFixtures.TCP_TRANSPORT_TIMEOUT)
@pytest.mark.asyncio
async def test_tcp_transport(certificate_file, http_server):
    """
    Starts an echoing http server, a injector and a client and send a unique http
    request to the echoing http server through the g client --tcp--> injector -> http server
    and make sure it gets the correct echo. The unique request makes sure that
    the response is from the http server and is not cached.
    """
    # It is client who will decide if there will be caching or not
    print("starting test")
    injector = run_tcp_injector(
        args=[
            "--listen-on-tcp",
            f"127.0.0.1:{TestFixtures.TCP_INJECTOR_PORT}",
        ]
    )

    # Wait for the injector to open port
    await wait_for_benchmark(injector, TestFixtures.TCP_INJECTOR_PORT_READY_REGEX)

    # Client
    client = run_tcp_client(
        name=TestFixtures.TCP_CLIENT["name"],
        args=[
            "--disable-origin-access",
            "--cache-type=none",  # Use only Proxy mechanism
            "--listen-on-tcp",
            f"127.0.0.1:{TestFixtures.TCP_CLIENT['port']}",
            "--injector-ep",
            f"tcp:127.0.0.1:{TestFixtures.TCP_INJECTOR_PORT}",
        ],
    )

    # Wait for the client to open port
    await wait_for_benchmark(client, TestFixtures.TCP_CLIENT_PORT_READY_REGEX)

    # TODO: No need to randomize in this particular test.
    # One can make another test to check that unique addresses are not cached
    content = safe_random_str(TestFixtures.RESPONSE_LENGTH)
    response: Reqresponse = request_echo(TestFixtures.TCP_CLIENT["port"], content)

    assertEquals(response.status_code, 200)
    assertEquals(response.text, content)


@pytest.mark.timeout(TestFixtures.BEP44_CACHE_TIMEOUT)
@pytest.mark.asyncio
async def test_tcp_cache(certificate_file, http_server):
    """
    Starts an echoing http server, a injector and a two clients and client1 send a unique http
    request to the echoing http server through the g client --tcp--> injector -> http server
    and make sure it gets the correct echo. The test waits for the response to be cached.
    Then the second client request the same request makes sure that
    the response is served from cache.
    """
    # Injector (caching by default)
    injector: OuinetBEP5CacheInjector = run_tcp_injector(
        ["--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)],
    )
    await wait_for_benchmark(injector, TestFixtures.TCP_INJECTOR_PORT_READY_REGEX)

    # Wait for the injector to have a key
    await wait_for_benchmark(injector, TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX)
    index_key = injector.get_index_key()
    assert len(index_key) > 0

    ## Client, use only Injector mechanism
    client = run_tcp_client(
        name=TestFixtures.CACHE_CLIENT[0]["name"],
        args=[
            "--cache-type",
            "bep5-http",
            "--cache-http-public-key",
            str(index_key),
            "--disable-origin-access",
            "--disable-proxy-access",
            "--listen-on-tcp",
            "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["port"]),
            "--front-end-ep",
            "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["fe_port"]),
            "--injector-ep",
            "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT),
        ],
    )

    # Wait for the client to open the port
    await wait_for_benchmark(client, TestFixtures.TCP_CLIENT_PORT_READY_REGEX)

    content = safe_random_str(TestFixtures.RESPONSE_LENGTH)
    response: Reqresponse = request_echo(TestFixtures.CACHE_CLIENT[0]["port"], content)
    assertEquals(response.status_code, 200)
    assertEquals(response.text, content)

    # Shut injector down to ensure it does not seed content to the cache client
    await injector.stop()

    # Now waiting for client to annouce caching the response
    await wait_for_benchmark(client, TestFixtures.CACHE_CLIENT_REQUEST_STORED_REGEX)

    # Start cache client which supposed to read the response from cache, use only Cache mechanism
    cache_client = run_tcp_client(
        TestFixtures.CACHE_CLIENT[1]["name"],
        args=[
            "--cache-type",
            "bep5-http",
            "--cache-http-public-key",
            str(index_key),
            "--disable-origin-access",
            "--disable-proxy-access",
            "--listen-on-tcp",
            "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"]),
            "--front-end-ep",
            "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["fe_port"]),
            "--injector-ep",
            "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT),
        ],
    )

    # Make sure that the client2 is ready to access the cache
    await wait_for_benchmark(cache_client, TestFixtures.TCP_CLIENT_DISCOVERY_START)
    await wait_for_dht_ready(cache_client)
    await wait_for_benchmark(cache_client, TestFixtures.DHT_CONTACTS_STORED_REGEX)

    # Now request the same page from second client
    for i in range(0, TestFixtures.MAX_NO_OF_TRIAL_CACHE_REQUESTS):
        try:
            response: Reqresponse = request_echo(
                TestFixtures.CACHE_CLIENT[1]["port"], content
            )
        except Exception:
            # print("[WARNING] failing to retrieve from cache with error", str(e))
            continue
        if response.status_code == 200:
            break

    assertEquals(response.status_code, 200)
    assertEquals(response.text, content)

    # # make sure it was served from cache
    await wait_for_benchmark(client, TestFixtures.CACHE_CLIENT_UTP_REQUEST_SERVED)
    await wait_for_benchmark(cache_client, TestFixtures.RESPONSE_RECEIVED_FROM_CACHE)


@pytest.mark.timeout(TestFixtures.BEP44_CACHE_TIMEOUT)
@pytest.mark.asyncio
async def test_wikipedia_mainline_dht(http_server, certificate_file):
    """
    A test to reach wikipedia without using our own injector
    """

    # Client
    client_port = TestFixtures.TCP_CLIENT["port"]
    frontend_port = TestFixtures.TCP_CLIENT["fe_port"]
    client = run_tcp_client(
        name=TestFixtures.TCP_CLIENT["name"],
        args=[
            "--disable-origin-access",
            "--cache-type=bep5-http",
            f"--cache-http-public-key={TestFixtures.MAINNET_INJECTOR_HASH}",
            "--listen-on-tcp",
            f"127.0.0.1:{client_port}",
            "--front-end-ep",
            f"127.0.0.1:{frontend_port}",
            "--injector-credentials",
            "ouinet:160d79874a52c2cbcdec58db1a8160a9",
            "--injector-tls-cert-file",
            TestFixtures.INJECTOR_CERT_PATH,
        ],
    )

    await wait_for_benchmark(client, TestFixtures.TCP_CLIENT_PORT_READY_REGEX)

    await wait_for_dht_ready(client)
    await wait_for_benchmark(client, TestFixtures.DHT_CONTACTS_STORED_REGEX)
    # Peer candidates will necessarily be after DHT storing contacts
    await wait_for_injector_peer_candidates(frontend_port)

    response = request_url(client_port, "http://example.org")

    if not response.status_code == 200:
        raise Exception(
            response.status_code, response.reason, response.text, response.request
        )

    # Confirm that it was fresh
    await wait_for_benchmark(client, TestFixtures.FRESH_SUCCESS_REGEX)
