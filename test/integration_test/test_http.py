# Copyright 2018, 2019 eQualit.ie, Inc.
# See LICENSE for other credits and copying information

# Integration tests for Ouinet - test for http communication offered through different transports and caches

import socket

# Making random requests not to relying on cache
import string
import random
import sys
import logging
from time import sleep  # XXX: remove later

from twisted.internet import reactor
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.defer import inlineCallbacks, Deferred, gatherResults

from twisted.web.client import ProxyAgent, readBody
from twisted.trial.unittest import TestCase
from twisted.internet import task

from ouinet_process_controler import (
    OuinetConfig,
    OuinetClient,
    OuinetBEP5CacheInjector,
)

from test_fixtures import TestFixtures
from test_http_server import TestHttpServer


class OuinetTests(TestCase):
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        logging.basicConfig(
            stream=sys.stderr,
            format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            level=TestFixtures.LOGGING_LEVEL,
        )

        # you can't set the timout in the test method itself :-(
        self.timeout = TestFixtures.TEST_TIMEOUT[self._testMethodName]
        logging.debug(
            "setting timeout for "
            + self._testMethodName
            + " at "
            + str(TestFixtures.TEST_TIMEOUT[self._testMethodName])
        )

        self.proc_list = []  # keep track of all process we start for clean tear down

    def safe_random_str(self, length):
        letters = string.ascii_lowercase
        return "".join(random.choice(letters) for i in range(length))

    def get_nonloopback_ip(self):
        """Return a local IP which is not loopback.

        This is needed since injectors may block such requests for security
        reasons.
        """
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 1))  # no actual traffic here
        ip = s.getsockname()[0]
        s.close()
        return ip

    def run_tcp_injector(self, injector_args):
        # BEP5 is our default injector
        injector = OuinetBEP5CacheInjector(
            OuinetConfig(
                app_name=TestFixtures.TCP_INJECTOR_NAME + "_tcp",
                timeout=TestFixtures.TCP_TRANSPORT_TIMEOUT,
                argv=injector_args,
                # TODO: move it to the class itself
                benchmark_regexes=[
                    TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX,
                    TestFixtures.TCP_INJECTOR_PORT_READY_REGEX,
                    TestFixtures.BEP5_REQUEST_CACHED_REGEX,
                ],
            ),
        )
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_tcp_client(self, name, args):
        client = OuinetClient(
            OuinetConfig(
                name,
                TestFixtures.TCP_TRANSPORT_TIMEOUT,
                args,
                benchmark_regexes=[
                    TestFixtures.TCP_CLIENT_PORT_READY_REGEX,
                    TestFixtures.TCP_CLIENT_DISCOVERY_START,
                ],
            ),
        )
        client.start()
        self.proc_list.append(client)

        return client

    def run_http_server(self, port):
        return TestHttpServer(port)

    def request_echo(self, port, content):
        """
        Send a get request to request the test server to echo the content
        """
        url = "http://%s:%d/?content=%s" % (
            self.get_nonloopback_ip(),
            TestFixtures.TEST_HTTP_SERVER_PORT,
            content,
        )
        return self.request_url(port, url)

    def request_page(self, port, page_url):
        """
        Send a get request to request test content as a page on a specific sub url
        """
        url = "http://%s:%d/%s" % (
            self.get_nonloopback_ip(),
            TestFixtures.TEST_HTTP_SERVER_PORT,
            page_url,
        )
        return self.request_url(port, url)

    def request_url(self, port, url):
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", port)
        agent = ProxyAgent(ouinet_client_endpoint)
        return agent.request(b"GET", url.encode())

    def _cache_injector_config(self, timeout, evt_regexes, args):
        return OuinetConfig(
            TestFixtures.CACHE_INJECTOR_NAME,
            timeout,
            args,
            benchmark_regexes=(
                [TestFixtures.TCP_INJECTOR_PORT_READY_REGEX] + evt_regexes
            ),
        )

    # TODO: they are the same, dedupe
    def _do_run_injector(self, proc_class, config, evt_deferreds):
        injector = proc_class(config, evt_deferreds)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def _do_run_client(self, proc_class, config, evt_deferreds):
        client = proc_class(config, evt_deferreds)
        client.start()
        self.proc_list.append(client)

        return client

    ################# Tests #####################

    @inlineCallbacks
    def _test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        logging.debug("################################################")
        logging.debug("test_tcp_transport")
        logging.debug("################################################")

        # Injector
        injector_ready = Deferred()

        # It is client who will decide if there will be caching or not
        self.run_tcp_injector(
            injector_args=[
                "--listen-on-tcp",
                f"127.0.0.1:{TestFixtures.TCP_INJECTOR_PORT}",
            ],
            deferred_ready=injector_ready,
        )

        # Wait for the injector to open the port
        success = yield injector_ready
        self.assertTrue(success)

        # Client
        client_tcp_port_ready = Deferred()
        self.run_tcp_client(
            name=TestFixtures.TCP_CLIENT["name"],
            args=[
                "--disable-origin-access",
                "--cache-type=none",  # Use only Proxy mechanism
                "--listen-on-tcp",
                f"127.0.0.1:{TestFixtures.TCP_CLIENT['port']}",
                "--injector-ep",
                f"tcp:127.0.0.1:{TestFixtures.TCP_INJECTOR_PORT}",
            ],
            deferred_tcp_port_ready=client_tcp_port_ready,
        )

        success = yield client_tcp_port_ready
        self.assertTrue(success)

        # Host a test page
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        # TODO: No need to randomize in this particular test.
        # One can make another test to check that unique addresses are not cached
        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        deferred_response = yield self.request_echo(
            TestFixtures.TCP_CLIENT["port"], content
        )

        self.assertEquals(deferred_response.code, 200)

        response_body = yield readBody(deferred_response)
        self.assertEquals(response_body.decode(), content)

    @inlineCallbacks
    def test_tcp_cache(self):
        """
        Starts an echoing http server, a injector and a two clients and client1 send a unique http
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The test waits for the response to be cached.
        Then the second client request the same request makes sure that
        the response is served from cache.
        """
        # Injector (caching by default)
        cache_injector: OuinetBEP5CacheInjector = self.run_tcp_injector(
            ["--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)],
        )

        # Wait for the injector to have a key
        success = yield cache_injector.callbacks[TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX]
        self.assertTrue(success)

        index_key = cache_injector.get_index_key()
        assert len(index_key) > 0

        # # Injector client, use only Injector mechanism
        client = self.run_tcp_client(
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
                "--injector-ep",
                "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT),
            ],
        )

        success = yield client.callbacks[TestFixtures.TCP_CLIENT_PORT_READY_REGEX]
        # Wait for the client to open the port
        self.assertTrue(success)

        # # Http_server

        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        page_url = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield self.request_page(
            TestFixtures.CACHE_CLIENT[0]["port"], page_url
        )
        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)

        # XXX
        injector_seed = True

        if injector_seed:
            # shut client down to ensure it does not seed content to the cache client
            client.stop()
            # now waiting or injector to annouce caching the request
            success = yield cache_injector.callbacks[
                TestFixtures.BEP5_REQUEST_CACHED_REGEX
            ]
            self.assertTrue(success)

            #        else:
            #            # shut injector down to ensure it does not seed content to the cache client
            #            cache_injector.stop()
            #            # now waiting for client to annouce caching the response
            #            success = yield client_cached_result
            #            self.assertTrue(success)

            # Start cache client which supposed to read the response from cache, use only Cache mechanism
            cache_client = self.run_tcp_client(
                TestFixtures.CACHE_CLIENT[1]["name"],
                [
                    "--cache-type",
                    "bep5-http",
                    "--cache-http-public-key",
                    str(index_key),
                    "--disable-origin-access",
                    "--disable-proxy-access",
                    "--listen-on-tcp",
                    "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"]),
                    "--injector-ep",
                    "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT),
                ],
            )
            sleep(7)

            import time

            # make sure that the client2 is ready to access the cache
            success = yield cache_client.callbacks[
                TestFixtures.TCP_CLIENT_DISCOVERY_START
            ]
            index_resolution_done_time_stamp = time.time()
            self.assertTrue(success)

            # try:
            #     index_resolution_start = cache_client.index_resolution_start_time()
            #     self.assertTrue(index_resolution_start > 0)

            #     logging.debug(
            #         "Index resolution took: "
            #         + str(index_resolution_done_time_stamp - index_resolution_start)
            #         + " seconds"
            #     )
            # except AttributeError:  # index has no global resolution
            #     pass

            # now request the same page from second client
            defered_response = Deferred()
            for i in range(0, TestFixtures.MAX_NO_OF_TRIAL_CACHE_REQUESTS):
                defered_response = yield self.request_page(
                    TestFixtures.CACHE_CLIENT[1]["port"], page_url
                )
                if defered_response.code == 200:
                    break
                yield task.deferLater(
                    reactor, TestFixtures.TRIAL_CACHE_REQUESTS_WAIT, lambda: None
                )

            self.assertEquals(defered_response.code, 200)

            sleep(5)

            response_body = yield readBody(defered_response)
            self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)

    #        # make sure it was served from cache
    #        self.assertTrue(cache_client.served_from_cache())

    def tearDown(self):
        deferred_procs = []
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()

        if hasattr(self, "test_http_server"):
            deferred_procs.append(self.test_http_server.stopListening())

        return gatherResults(deferred_procs)
