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
import time

from twisted.internet import reactor
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.defer import inlineCallbacks, Deferred, gatherResults

from twisted.web.client import ProxyAgent, readBody
from twisted.web.http_headers import Headers
from twisted.trial.unittest import TestCase
from twisted.internet import task

from ouinet_process_controler import (
    OuinetInjector,
    OuinetI2PInjector,
    OuinetConfig,
    OuinetClient,
    OuinetBEP5CacheInjector,
)

from test_fixtures import TestFixtures
from test_http_server import TestHttpServer

import sys
import logging
import time

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

    def run_tcp_injector(self, args):
        argv = args.copy()
        argv.append("--allow-private-targets")
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
        self.proc_list.append(injector)

        return injector

    def run_i2p_injector(self, args):
        argv = args.copy()
        argv.append("--allow-private-targets")
        injector = OuinetI2PInjector(
            OuinetConfig(app_name = TestFixtures.I2P_INJECTOR_NAME + "_i2p",
                         timeout = TestFixtures.I2P_TRANSPORT_TIMEOUT,
                         argv = argv,
                         benchmark_regexes=[
                             TestFixtures.I2P_TUNNEL_READY_REGEX]))
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_i2p_injector_with_cache_pub_key(self, injector_args, deferred_cache_pub_key, deferred_i2p_ready):
        argv = args.copy()
        argv.append("--allow-private-targets")

        injector = OuinetI2PInjector(
            OuinetConfig(app_name=TestFixtures.I2P_INJECTOR_NAME + "_i2p",
                         timeout=TestFixtures.I2P_TRANSPORT_TIMEOUT,
                         argv=argv,
                         benchmark_regexes=[TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX,
                                            TestFixtures.I2P_TUNNEL_READY_REGEX]))
        injector.start()
        self.proc_list.append(injector)

        return injector

    def _cache_injector_config(self, timeout, evt_regexes, args):
        return OuinetConfig(TestFixtures.CACHE_INJECTOR_NAME, timeout, args,
                            benchmark_regexes=([TestFixtures.TCP_PORT_READY_REGEX] + evt_regexes))

    def _run_cache_injector(self, proc_class, config, evt_deferreds):
        injector = proc_class(config, evt_deferreds)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_tcp_client(self, name, args):
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
                ],
            ),
        )
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_client(self, name, idx_key, args):
        client = OuinetClient(
            OuinetConfig(app_name=name,
                         timeout=TestFixtures.I2P_TRANSPORT_TIMEOUT,
                         argv=argv,
                         benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]))
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_bep5_client(self, name, idx_key, args, deferred_i2p_tunneller_ready, deferred_i2p_client_finished_reading):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.I2P_TUNNELLER_LISTENING_REGEX, TestFixtures.I2P_CLIENT_FINISHED_READING_REGEX]), [deferred_i2p_tunneller_ready, deferred_i2p_client_finished_reading])
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

    def request_sized_content(self, port, content_size):
        """
        Send a get request to request the test server to send a random content of a specific size
        """
        url = "http://%s:%d/?content_size=%s" % (
            self.get_nonloopback_ip(), TestFixtures.TEST_HTTP_SERVER_PORT, str(content_size)
        )
        return self.request_url(port, url)

    def request_page(self, port, page_url, cachable = False):
        """
        Send a get request to request test content as a page on a specific sub url
        """
        url = "http://%s:%d/%s" % (
            self.get_nonloopback_ip(),
            TestFixtures.TEST_HTTP_SERVER_PORT,
            page_url,
        )
        return self.request_url(port, url)

    def request_url(self, port, url, cachable = True):
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", port)
        agent = ProxyAgent(ouinet_client_endpoint)
        headers = cachable and Headers({b"X-Ouinet-Group": [self.get_nonloopback_ip()] }) or None
        return agent.request(b"GET", url.encode(), headers)

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


    @inlineCallbacks
    def test_externally_discovered_i2p_injector(self):
        """
        Starts an echoing http server which server a constant size response, an injector and a client.
        the client listen on a tcp port to receive the injector's id thne send a http 
        request to the echoing http server through the client --i2p--> injector -> http server
        and make sure it gets the response.
        """
        logging.debug("################################################")
        logging.debug("test_externally_discovered_i2p_injector");
        logging.debug("################################################")
        # #injector
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector(["--listen-on-i2p", "true", "--log-level", "DEBUG",
                                             ], [i2pinjector_tunnel_ready]) #"--disable-cache"


        #wait for the injector tunnel to be advertised
        success = yield i2pinjector_tunnel_ready

        #we only can request that after injector is ready
        injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
        # injector_i2p_public_id = TestFixtures.INJECTOR_I2P_PUBLIC_ID
        self.assert_(injector_i2p_public_id) # empty public id means injector coludn't read the endpoint file

        # Wait so the injector id gets advertised on the DHT
        logging.debug("waiting " + str(TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD) + " secs for the tunnel to get advertised on the DHT...")
        yield task.deferLater(reactor, TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD, lambda: None)
        
        # Http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #client
        test_passed = False
        for i2p_client_id in range(0, 10):  # TestFixtures.MAX_NO_OF_I2P_CLIENTS):
            i2p_tunneller_ready = defer.Deferred()                        
            i2p_client_finished_reading = defer.Deferred()

            #use only Proxy or Injector mechanisms
            self.run_i2p_bep5_client( TestFixtures.I2P_CLIENT["name"], None
                               , [ "--disable-origin-access"
                                 , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                   , "--cache-type", "bep3-http-over-i2p",
                                   "--log-level", "DEBUG",
                                  ]
                                 ,)

            #wait for the cache discovery tunnel get open
            success = yield i2p_tunneller_ready
            if TestFixtures.SIMULATE_I2P_EXTERNAL_DISCOVERY:
                self.write_into_tcp_socket(TestFixtures.I2P_DISCOVERED_ID_ANNOUNCE_PORT, injector_i2p_public_id + "\n")

            #wait for the client tunnel to connect to the injector and request content
            #The client should write a message which make us know that the end of content
            #is acheived and we yeild based on that
            try:
                success = yield i2p_client_finished_reading
                break
            except Exception as err:
                yield i2pclient.proc_end
                yield task.deferLater(reactor, TestFixtures.I2P_TUNNEL_HEALING_PERIOD, lambda: None)


        self.assertTrue(success)

    @inlineCallbacks
    def test_i2p_transport(self):
        return self._test_i2p_transport(None)

    @inlineCallbacks
    def test_i2p_transport_speed_1MB(self):
        return self._test_i2p_transport(size_of_transported_blob = 1024 * 1024)

    @inlineCallbacks
    def test_i2p_transport_speed_1KB(self):
        return self._test_i2p_transport(size_of_transported_blob = 1024)

    def _test_i2p_transport(self, size_of_transported_blob = None):
        """
        Starts an echoing http server, an injector and a client and send a unique http 
        request to the echoing http server through the client --i2p--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        logging.debug("################################################")
        logging.debug("test_i2p_transport");
        logging.debug("################################################")
        # injector events
        i2pinjector_tunnel_ready = defer.Deferred()

        i2pinjector = self.run_i2p_injector(args=["--listen-on-i2p", "true",
                                             "--i2p-hops-per-tunnel", str(TestFixtures.I2P_ANON_TUNNEL_HOP_COUNT),
                                             "--log-level", "DEBUG",
                                             ]) #"--disable-cache"


        #wait for the injector tunnel to be advertised
        success = yield i2pinjector.callbacks[TestFixtures.I2P_TUNNEL_READY_REGEX]
        self.assertTrue(success)

        #we only can request that after injector is ready
        injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
        # injector_i2p_public_id = TestFixtures.INJECTOR_I2P_PUBLIC_ID #fake injector
        self.assert_(injector_i2p_public_id) #empty public id means injector coludn't read the endpoint file

        #wait so the injector id gets advertised on the DHT
        logging.debug("waiting " + str(TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD) + " secs for the tunnel to get advertised on the DHT...")
        yield task.deferLater(reactor, TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD, lambda: None)
        
        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #client
        test_passed = False
        request_start = None
        response_end = None
        for i2p_client_id in range(0, TestFixtures.MAX_NO_OF_I2P_CLIENTS):
            #use only Proxy or Injector mechanisms
            current_client = self.run_i2p_client( name=TestFixtures.I2P_CLIENT["name"], idx=None
                               , args=[ "--disable-origin-access", "--disable-cache"
                                 , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                   , "--injector-ep", "i2p:" + injector_i2p_public_id
                                   , "--i2p-hops-per-tunnel", str(TestFixtures.I2P_ANON_TUNNEL_HOP_COUNT)
                                   , "--log-level", "DEBUG",
                                 ]
                               )
        
            #wait for the client tunnel to connect to the injector
            success = yield current_client.callbacks[TestFixtures.I2P_TUNNEL_READY_REGEX]

            content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
            for i in range(0,TestFixtures.MAX_NO_OF_TRIAL_I2P_REQUESTS):
                logging.debug("request attempt no " + str(i+1) + "...")

                request_start = time.time()
                if (size_of_transported_blob == None):
                    defered_response = yield  self.request_echo(TestFixtures.I2P_CLIENT["port"], content)
                else:
                    defered_response = yield  self.request_sized_content(TestFixtures.I2P_CLIENT["port"], size_of_transported_blob)
                if defered_response.code == 200:
                    response_body = yield readBody(defered_response)
                    response_end = time.time()
                    actual_response_length = TestFixtures.RESPONSE_LENGTH
                    if size_of_transported_blob == None:
                        self.assertEquals(response_body.decode(), content)
                    else:
                        actual_response_length = size_of_transported_blob

                    print(f"Retrieving %.3e bytes through I2P tunnel took %f seconds." % (actual_response_length, response_end - request_start))

                    if size_of_transported_blob != None and i == 0:
                        #if it is a speed test disregard the first attempt
                        logging.debug("repeating speed test for more accurate speed test result...")
                    else:
                        test_passed = True
                        break

                else:
                    logging.debug("request attempt no " + str(i+1) + " failed. with code " + str(defered_response.code))
                    yield task.deferLater(reactor, TestFixtures.I2P_TUNNEL_HEALING_PERIOD, lambda: None)

            if test_passed:
                break
            else:
                #stop the i2p client so we can start a new one
                i2pclient = self.proc_list.pop()
                yield i2pclient.proc_end

        self.assertTrue(test_passed)

    @inlineCallbacks
    def test_bep5_caching_of_i2p_served_content(self):
        """
        Starts an echoing http server, an injector and client1 and send a unique http 
        request to the echoing http server through the client1 --i2p--> injector -> http server
        and make sure it gets the correct echo. Then start client2 which does not know the injecter
        and request the same url over bep5 dht and the test makes sure that client2 also gets
        the content
        """
        logging.debug("################################################")
        logging.debug("test_bep5_caching_of_i2p_served_content");
        logging.debug("################################################")
        # #injector
        # index_key = "c42844c552b8f7c24493496d7e0978896f87f34ab2690a83bf951dddf777267e"
        # client_cache_ready = defer.Deferred() 
        # cache_client = self.run_bep5_client(
        #     TestFixtures.CACHE_CLIENT[1]["name"], index_key,
        #     [ "--disable-origin-access", "--disable-proxy-access"
        #     , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"])
        #     ,  "--front-end-ep", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["front-end-port"])
        #     , "--log-level", "DEBUG",

        #     ],
        #     client_cache_ready)
            
        i2pinjector_cache_key_announced = defer.Deferred()
        i2pinjector_cache_key_announced.addCallback(OuinetTests.look_for_BEP5_pubk)
        
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector_with_cache_pub_key(["--listen-on-i2p", "true",
                                             "--i2p-hops-per-tunnel", str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
                                             "--log-level", "DEBUG",
                                                                 ], i2pinjector_cache_key_announced, i2pinjector_tunnel_ready) #"--disable-cache"

        success = yield i2pinjector_cache_key_announced        
        index_key = i2pinjector.get_index_key()
        logging.debug("Index key is: " + index_key)
        #wait for the injector tunnel to be advertised
        success = yield i2pinjector_tunnel_ready

        #we only can request that after injector is ready
        injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
        # injector_i2p_public_id = TestFixtures.INJECTOR_I2P_PUBLIC_ID
        self.assert_(injector_i2p_public_id) #empty public id means injector coludn't read the endpoint file

        #wait so the injector id gets advertised on the DHT
        logging.debug("waiting " + str(TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD) + " secs for the tunnel to get advertised on the DHT...")
        yield task.deferLater(reactor, TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD, lambda: None)
        
        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #client

        content_delivered_over_i2p = False
        for i2p_client_id in range(0, TestFixtures.MAX_NO_OF_I2P_CLIENTS):
            i2pclient_tunnel_ready = defer.Deferred()

            #use only Proxy or Injector mechanisms
            self.run_i2p_client( TestFixtures.I2P_CLIENT["name"], None
                               , [ "--disable-origin-access",
                                   "--cache-type", "bep5-http",
                                   "--cache-private",
                                   "--cache-http-public-key", index_key,
                                   "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"]),
                                   "--injector-ep", "i2p:" + injector_i2p_public_id,
                                   "--i2p-hops-per-tunnel", str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
                                   "--log-level", "DEBUG",
                                 ]
                               , i2pclient_tunnel_ready)
        
            #wait for the client tunnel to connect to the injector
            success = yield i2pclient_tunnel_ready

            content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
            for i in range(0, TestFixtures.MAX_NO_OF_TRIAL_I2P_REQUESTS):
                logging.debug("request attempt no " + str(i+1) + "...")
                defered_response = yield  self.request_page(TestFixtures.I2P_CLIENT["port"], content, cachable=True)
                if defered_response.code == 200:
                    response_body = yield readBody(defered_response)
                    self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)
                    content_delivered_over_i2p = True
                    break
                
                else:
                    logging.debug("request attempt no " + str(i+1) + " failed. with code " + str(defered_response.code))
                    yield task.deferLater(reactor, TestFixtures.I2P_TUNNEL_HEALING_PERIOD, lambda: None)

            if content_delivered_over_i2p:
                break
            else:
                #stop the i2p client so we can start a new one
                i2pclient = self.proc_list.pop()
                yield i2pclient.proc_end()

        self.assertTrue(content_delivered_over_i2p)

                #start cache client which supposed to read the response from cache, use only Cache mechanism
        client_cache_ready = defer.Deferred() 
        cache_client = self.run_bep5_client(
            TestFixtures.CACHE_CLIENT[1]["name"], index_key,
            [ "--disable-origin-access", "--disable-proxy-access"
            , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"])
            , "--front-end-ep", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["front-end-port"])
            , "--log-level", "DEBUG",
            ],
        )
        sleep(7)

        # make sure that the client2 is ready to access the cache
        success = yield client_cache_ready
        self.assertTrue(success)

        # now request the same page from second client
        defered_response = Deferred()
        for i in range(0, TestFixtures.MAX_NO_OF_TRIAL_CACHE_REQUESTS):
            defered_response = yield self.request_page(
                TestFixtures.CACHE_CLIENT[1]["port"], content)
            if (defered_response.code == 200):
                break
            yield task.deferLater(
                reactor, TestFixtures.TRIAL_CACHE_REQUESTS_WAIT, lambda: None
            )

        self.assertEquals(defered_response.code, 200)

        sleep(5)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)

        print("all ok, now waiting")

        # make sure it was served from cache
        self.assertTrue(cache_client.served_from_cache())
        
        
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
    def test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        logging.debug("################################################")
        logging.debug("test_tcp_transport")
        logging.debug("################################################")

        # It is client who will decide if there will be caching or not
        injector = self.run_tcp_injector(
            args=[
                "--listen-on-tcp",
                f"127.0.0.1:{TestFixtures.TCP_INJECTOR_PORT}",
            ],
        )

        # Wait for the injector to open port
        success = yield injector.callbacks[TestFixtures.TCP_INJECTOR_PORT_READY_REGEX]
        self.assertTrue(success)

        # Client
        client = self.run_tcp_client(
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

        success = yield client.callbacks[TestFixtures.TCP_CLIENT_PORT_READY_REGEX]
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
                "--front-end-ep",
                "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["fe_port"]),
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

        # shut injector down to ensure it does not seed content to the cache client
        cache_injector.stop()
        # now waiting for client to annouce caching the response

        sleep(10)
        success = yield client.callbacks[TestFixtures.CACHE_CLIENT_REQUEST_STORED_REGEX]
        self.assertTrue(success)

        # Start cache client which supposed to read the response from cache, use only Cache mechanism
        cache_client = self.run_tcp_client(
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
        sleep(7)

        # import time

        # make sure that the client2 is ready to access the cache
        success = yield cache_client.callbacks[TestFixtures.TCP_CLIENT_DISCOVERY_START]
        index_resolution_done_time_stamp = time.time()
        self.assertTrue(success)

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

        print("all ok, now waiting")

        # make sure it was served from cache
        success = yield client.callbacks[TestFixtures.CACHE_CLIENT_UTP_REQUEST_SERVED]
        self.assertTrue(success)

    def tearDown(self):
        deferred_procs = []
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()

        if hasattr(self, "test_http_server"):
            deferred_procs.append(self.test_http_server.stopListening())

        return gatherResults(deferred_procs)
