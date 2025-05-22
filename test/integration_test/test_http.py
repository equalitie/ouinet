# Copyright 2018, 2019 eQualit.ie, Inc.
# See LICENSE for other credits and copying information

# Integration tests for Ouinet - test for http communication offered through different transports and caches
import os
import os.path
import re

from twisted.internet import reactor, defer, task
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.defer import inlineCallbacks
from twisted.trial.unittest import TestCase
from twisted.web.client import ProxyAgent, readBody
from twisted.web.http_headers import Headers

import socket

#making random requests not to relying on cache
import string
import random

import pdb

from ouinet_process_controler import OuinetConfig
from ouinet_process_controler import (
    OuinetInjector, OuinetI2PInjector, OuinetIPFSCacheInjector, OuinetBEP5CacheInjector)
from ouinet_process_controler import (
    OuinetClient, OuinetIPFSClient, OuinetBEP5Client)
from test_fixtures import TestFixtures
from test_http_server import TestHttpServer

import sys
import logging

class OuinetTests(TestCase):
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        logging.basicConfig(stream=sys.stderr, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=TestFixtures.LOGGING_LEVEL, )

        #you can't set the timout in the test method itself :-(
        self.timeout = TestFixtures.TEST_TIMEOUT[self._testMethodName]
        logging.debug("setting timeout for " + self._testMethodName + " at " + str(TestFixtures.TEST_TIMEOUT[self._testMethodName]))

        self.proc_list = [] #keep track of all process we start for clean tear down

    def safe_random_str(self, length):
        letters = string.ascii_lowercase
        return ''.join(random.choice(letters) for i in range(length))

    def get_nonloopback_ip(self):
        """Return a local IP which is not loopback.

        This is needed since injectors may block such requests for security
        reasons.
        """
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('192.0.2.1', 1))  # no actual traffic here
        ip = s.getsockname()[0]
        s.close()
        return ip
    
    def run_tcp_injector(self, injector_args, deffered_tcp_port_ready):
        injector = OuinetInjector(OuinetConfig(app_name=TestFixtures.TCP_INJECTOR_NAME + "_tcp", timeout=TestFixtures.TCP_TRANSPORT_TIMEOUT, argv=injector_args, benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX]), [deffered_tcp_port_ready])
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_i2p_injector(self, injector_args, deferred_i2p_ready):
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME + "_i2p", TestFixtures.I2P_TRANSPORT_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX, TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready])
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_i2p_injector_with_cache_pub_key(self, injector_args, deferred_cache_pub_key, deferred_i2p_ready):
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME + "_i2p", TestFixtures.I2P_TRANSPORT_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX, TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_cache_pub_key, deferred_i2p_ready])
        injector.start()
        self.proc_list.append(injector)

        return injector

    @staticmethod
    def look_for_BEP5_pubk(ouinet_process_protocol):
        BEP5_pubk_search_result = re.match(TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX, ouinet_process_protocol.ready_data)
        if BEP5_pubk_search_result:
            ouinet_process_protocol.BEP5_pubk = BEP5_pubk_search_result.group(1)

    def run_bep5_injector(self, injector_args,
                           deferred_tcp_port_ready, deferred_index_ready, deferred_result_got_cached):
        config = self._cache_injector_config(TestFixtures.BEP5_CACHE_TIMEOUT,
                                             [TestFixtures.BEP5_CACHE_READY_REGEX,
                                              TestFixtures.BEP5_REQUEST_CACHED_REGEX],
                                             injector_args)
        return self._run_cache_injector(
            OuinetBEP5CacheInjector, config,
            [deferred_tcp_port_ready, deferred_index_ready, deferred_result_got_cached])

    def run_bep5_signer(self, injector_args,
                         deferred_tcp_port_ready, deferred_index_ready):
        config = self._cache_injector_config(TestFixtures.BEP5_CACHE_TIMEOUT,
                                             [TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX],  # bootstrap not needed
                                             ["--seed-content", "0"] + injector_args)
        return self._run_cache_injector(
            OuinetBEP5CacheInjector, config,
            [deferred_tcp_port_ready, deferred_index_ready])

    def _cache_injector_config(self, timeout, evt_regexes, args):
        return OuinetConfig(TestFixtures.CACHE_INJECTOR_NAME, timeout, args,
                            benchmark_regexes=([TestFixtures.TCP_PORT_READY_REGEX] + evt_regexes))

    def _run_cache_injector(self, proc_class, config, evt_deferreds):
        injector = proc_class(config, evt_deferreds)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_tcp_client(self, name, idx_key, args, deffered_tcp_port_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.TCP_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX]), [deffered_tcp_port_ready])
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_client(self, name, idx_key, args, deferred_i2p_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready])
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_bep5_client(self, name, idx_key, args, deferred_i2p_tunneller_ready, deferred_i2p_client_finished_reading):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.I2P_TUNNELLER_LISTENING_REGEX, TestFixtures.I2P_CLIENT_FINISHED_READING_REGEX]), [deferred_i2p_tunneller_ready, deferred_i2p_client_finished_reading])
        client.start()
        self.proc_list.append(client)

        return client

    def run_bep5_client(self, name, idx_key, args, deferred_cache_ready):
        config = OuinetConfig(name, TestFixtures.BEP5_CACHE_TIMEOUT,
                              ["--cache-http-public-key", idx_key, "--cache-type", "bep5-http"] + args,
                              benchmark_regexes=[TestFixtures.BEP5_CACHE_READY_REGEX])
        return self._run_cache_client(OuinetBEP5Client, config, [deferred_cache_ready])

    def run_bep5_seeder(self, name, idx_key, args, deferred_cache_ready, deferred_result_got_cached):
        config = OuinetConfig(name, TestFixtures.BEP5_CACHE_TIMEOUT,
                              ["--cache-http-public-key", idx_key, "--cache-type", "bep5-http"] + args,
                              benchmark_regexes=[TestFixtures.BEP5_CACHE_READY_REGEX,
                                                 TestFixtures.BEP5_RESPONSE_CACHED_REGEX])
        return self._run_cache_client(OuinetBEP5Client, config,
                                      [deferred_cache_ready, deferred_result_got_cached])

    def _run_cache_client(self, proc_class, config, evt_deferreds):
        client = proc_class(config, evt_deferreds)
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
            self.get_nonloopback_ip(), TestFixtures.TEST_HTTP_SERVER_PORT, content
        )
        return self.request_url(port, url)

    def request_page(self, port, page_url, cachable = False):
        """
        Send a get request to request test content as a page on a specific sub url
        """
        url = "http://%s:%d/%s" % (
            self.get_nonloopback_ip(), TestFixtures.TEST_HTTP_SERVER_PORT, page_url
        )
        return self.request_url(port, url)
    
    def request_url(self, port, url, cachable = True):
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", port)
        agent = ProxyAgent(ouinet_client_endpoint)
        headers = cachable and Headers({b"X-Ouinet-Group": [b"1"] }) or None
        return agent.request(b"GET", url.encode(), headers)

    def try_to_connect_to_tcp_port(self, port):
        success = False
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
        try:
            sock.connect(("127.0.0.1", port))
            logging.debug(f"Connected to port {port}")
            success = True

        finally:
            sock.close()

        return success

    def write_into_tcp_socket(self, port, message):
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
        try:
            sock.connect(("127.0.0.1", port))
            logging.debug(f"Connected to port {port}")

            sock.sendall(message.encode('utf-8'))
            logging.debug(f"Sent: {message}")

        finally:
            sock.close()

    @inlineCallbacks
    def test_i2p_i2cp_server(self):
        """
        Starts a client and check if i2cp port is open
        """
        logging.debug("################################################")
        logging.debug("test_i2p_server");
        logging.debug("################################################")

        #client
        i2p_tunneller_ready = defer.Deferred()                        
        i2p_client_finished_reading = defer.Deferred()

        #use only Proxy or Injector mechanisms
        self.run_i2p_bep5_client( TestFixtures.I2P_CLIENT["name"], None
                                   , [ "--disable-origin-access"
                                       , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                       , "--cache-type", "bep3-http-over-i2p",
                                       "--log-level", "DEBUG",
                                      ]
                                   , i2p_tunneller_ready, i2p_client_finished_reading)

        #wait for the cache discovery tunnel get open
        success = yield i2p_tunneller_ready

        #now we simply try to connect to i2cp port and if so, the test is deemed successful
        self.assertTrue(self.try_to_connect_to_tcp_port(TestFixtures.I2P_CLIENT["i2cp_port"]))

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
                                 , i2p_tunneller_ready, i2p_client_finished_reading)

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

    def test_i2p_transport(self):
        self._test_i2p_transport(self)

    def test_i2p_transport_speed_1MB(self):
        self._test_i2p_transport(self, 1024 * 1024)

    @inlineCallbacks
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
        # #injector
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector(["--listen-on-i2p", "true",
                                             "--i2p-hops-per-tunnel", str(TestFixtures.I2P_ANON_TUNNEL_HOP_COUNT),
                                             "--log-level", "DEBUG",
                                             ], i2pinjector_tunnel_ready) #"--disable-cache"

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
        test_passed = False
        for i2p_client_id in range(0, TestFixtures.MAX_NO_OF_I2P_CLIENTS):
            i2pclient_tunnel_ready = defer.Deferred()

            #use only Proxy or Injector mechanisms
            self.run_i2p_client( TestFixtures.I2P_CLIENT["name"], None
                               , [ "--disable-origin-access", "--disable-cache"
                                 , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                   , "--injector-ep", "i2p:" + injector_i2p_public_id
                                   , "--i2p-hops-per-tunnel", str(TestFixtures.I2P_ANON_TUNNEL_HOP_COUNT)
                                   , "--log-level", "DEBUG",
                                 ]
                               , i2pclient_tunnel_ready)
        
            #wait for the client tunnel to connect to the injector
            success = yield i2pclient_tunnel_ready

            content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
            for i in range(0,TestFixtures.MAX_NO_OF_TRIAL_I2P_REQUESTS):
                logging.debug("request attempt no " + str(i+1) + "...")

                if (size_of_tranported_blob == None):
                    defered_response = yield  self.request_echo(TestFixtures.I2P_CLIENT["port"], content)
                else:
                    defered_response = yield  self.request_sized_content(TestFixtures.I2P_CLIENT["port"], size_of_transported_blob)
                if defered_response.code == 200:
                    response_body = yield readBody(defered_response)
                    self.assertEquals(response_body.decode(), content)
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
        print("Index key is: " + index_key)
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
                yield i2pclient.proc_end

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
            client_cache_ready)

        # make sure that the client2 is ready to access the cache
        success = yield client_cache_ready
        self.assertTrue(success)

        try:
            index_resolution_start = cache_client.index_resolution_start_time()
            self.assertTrue(index_resolution_start > 0)

            logging.debug("Index resolution took: " + str(
                index_resolution_done_time_stamp -
                index_resolution_start) + " seconds")
        except AttributeError:  # index has no global resolution
            pass

        # now request the same page from second client
        defered_response = defer.Deferred()
        for i in range(0,TestFixtures.MAX_NO_OF_TRIAL_CACHE_REQUESTS):
            defered_response = yield self.request_page(
                TestFixtures.CACHE_CLIENT[1]["port"], content)
            if (defered_response.code == 200):
                break
            yield task.deferLater(reactor, TestFixtures.TRIAL_CACHE_REQUESTS_WAIT, lambda: None)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)

        # make sure it was served from cache
        self.assertTrue(cache_client.served_from_cache())
        

    @inlineCallbacks
    def _test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        logging.debug("################################################")
        logging.debug("test_tcp_transport");
        logging.debug("################################################")
        #injector
        injector_tcp_port_ready = defer.Deferred()
        self.run_tcp_injector(["--listen-on-i2p", "false",
                               "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT),
                               "--disable-cache"], injector_tcp_port_ready)

        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #client
        client_tcp_port_ready = defer.Deferred()

        #use only Proxy or Injector mechanisms
        self.run_tcp_client( TestFixtures.TCP_CLIENT["name"], None
                           , [ "--disable-origin-access", "--cache-type=none"
                             , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_CLIENT["port"])
                             , "--injector-ep", "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)
                             ]
                           , client_tcp_port_ready)

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the client to open the port
        success = yield client_tcp_port_ready

        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield  self.request_echo(TestFixtures.TCP_CLIENT["port"], content)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body.decode(), content)

    # Disabled for the same reason as the above test.
    @inlineCallbacks
    def _test_bep5_cache(self):
        logging.debug("################################################")
        logging.debug("test_bep5_cache");
        logging.debug("################################################")
        return self._test_cache(self.run_bep5_injector, self.run_tcp_client, self.run_bep5_client)

    @inlineCallbacks
    def _test_bep5_seed(self):
        logging.debug("################################################")
        logging.debug("test_bep5_seed");
        logging.debug("################################################")
        return self._test_cache(self.run_bep5_signer, self.run_bep5_seeder, self.run_bep5_client,
                                injector_seed=False)

    def _test_cache(self, run_injector, run_client, run_cache_client, injector_seed=True):
        """
        Starts an echoing http server, a injector and a two clients and client1 send a unique http 
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The test waits for the response to be cached. 
        Then the second client request the same request makes sure that
        the response is served from cache.
        """
        #injector
        injector_tcp_port_ready = defer.Deferred()
        injector_index_ready = defer.Deferred()
        injector_cached_result = defer.Deferred()
        cache_injector = run_injector(
            [ "--listen-on-i2p", "false"
            , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)
            ]
            , injector_tcp_port_ready, injector_index_ready
            , *([injector_cached_result] if injector_seed else [])
        )
        
        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #wait for the index to be ready
        success = yield injector_index_ready
        self.assertTrue(success)

        index_key = cache_injector.get_index_key()
        assert(len(index_key) > 0);
        
        print("Index key is: " + index_key)

        #injector client, use only Injector mechanism
        client_ready = defer.Deferred()
        client_cached_result = defer.Deferred()
        client = run_client( TestFixtures.CACHE_CLIENT[0]["name"], index_key
                             , [ "--disable-origin-access", "--disable-proxy-access"
                               , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["port"])
                               ,  "--front-end-ep", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["front-end-port"])                                 
                               , "--injector-ep", "tcp:127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)
                               ] + (["--disable-cache"] if injector_seed else [])
                             , client_ready
                             , *([] if injector_seed else [client_cached_result]))

        #http_server
        self.test_http_server = self.run_http_server(
            TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the client to open the port
        success = yield defer.gatherResults([client_ready])
        self.assertTrue(success)

        page_url = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield self.request_page(TestFixtures.CACHE_CLIENT[0]["port"], page_url)
        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)
        
        if injector_seed:
            # shut client down to ensure it does not seed content to the cache client
            client.stop()
            # now waiting or injector to annouce caching the request
            success = yield injector_cached_result
            self.assertTrue(success)
        else:
            # shut injector down to ensure it does not seed content to the cache client
            cache_injector.stop()
            # now waiting for client to annouce caching the response
            success = yield client_cached_result
            self.assertTrue(success)

        #start cache client which supposed to read the response from cache, use only Cache mechanism
        client_cache_ready = defer.Deferred() 
        cache_client = self._run_cache_client(
            TestFixtures.CACHE_CLIENT[1]["name"], index_key,
            [ "--disable-origin-access", "--disable-proxy-access"
            , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"])
            ,  "--front-end-ep", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["front-end-port"])              
            ],
            client_cache_ready)

        import time

        # make sure that the client2 is ready to access the cache
        success = yield client_cache_ready
        index_resolution_done_time_stamp = time.time()
        self.assertTrue(success)

        try:
            index_resolution_start = cache_client.index_resolution_start_time()
            self.assertTrue(index_resolution_start > 0)

            logging.debug("Index resolution took: " + str(
                index_resolution_done_time_stamp -
                index_resolution_start) + " seconds")
        except AttributeError:  # index has no global resolution
            pass

        # now request the same page from second client
        defered_response = defer.Deferred()
        for i in range(0,TestFixtures.MAX_NO_OF_TRIAL_CACHE_REQUESTS):
            defered_response = yield self.request_page(
                TestFixtures.CACHE_CLIENT[1]["port"], page_url)
            if (defered_response.code == 200):
                break
            yield task.deferLater(reactor, TestFixtures.TRIAL_CACHE_REQUESTS_WAIT, lambda: None)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)

        # make sure it was served from cache
        self.assertTrue(cache_client.served_from_cache())

    def tearDown(self):
        deferred_procs = []
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()

        if hasattr(self, 'test_http_server'):
            deferred_procs.append(self.test_http_server.stopListening())
            
        return defer.gatherResults(deferred_procs)
