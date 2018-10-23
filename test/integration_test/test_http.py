# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for Ouinet - test for http communication offered through different transports and caches
import os
import os.path

from twisted.internet import reactor, defer, task
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.protocol import Protocol
from twisted.web.client import ProxyAgent, Agent, readBody
from twisted.web.http_headers import Headers
from twisted.internet.defer import inlineCallbacks, Deferred

from twisted.trial.unittest import TestCase
from twisted.internet.base import DelayedCall

import socket
import urllib

#making random requests not to relying on cache
import string
import random

import pdb

from ouinet_process_controler import OuinetConfig
from ouinet_process_controler import OuinetInjector, OuinetI2PInjector, OuinetIPFSCacheInjector
from ouinet_process_controler import OuinetClient, OuinetIPFSClient
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
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME + "_i2p", TestFixtures.I2P_TRANSPORT_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready], TestFixtures.I2P_REUSE_PREDEFINED_IDENTITY and TestFixtures.INJECTOR_I2P_PRIVATE_KEY or None)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_cache_injector(self, injector_args, deferred_tcp_port_ready, deferred_result_got_cached):
        injector = OuinetIPFSCacheInjector(OuinetConfig(TestFixtures.CACHE_INJECTOR_NAME, TestFixtures.IPFS_CACHE_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX, TestFixtures.REQUEST_CACHED_REGEX]), [deferred_tcp_port_ready, deferred_result_got_cached])
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_tcp_client(self, name, args, deffered_tcp_port_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.TCP_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX]), [deffered_tcp_port_ready])
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_client(self, name, args, deferred_i2p_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_TRANSPORT_TIMEOUT, args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready])
        client.start()
        self.proc_list.append(client)

        return client

    def run_cache_client(self, name, args, deferred_cache_ready):
        client = OuinetIPFSClient(OuinetConfig(name, TestFixtures.IPFS_CACHE_TIMEOUT, args, benchmark_regexes=[TestFixtures.IPFS_CACHE_READY_REGEX]), [deferred_cache_ready])
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

    def request_page(self, port, page_url):
        """
        Send a get request to request test content as a page on a specific sub url
        """
        url = "http://%s:%d/%s" % (
            self.get_nonloopback_ip(), TestFixtures.TEST_HTTP_SERVER_PORT, page_url
        )
        return self.request_url(port, url)
    
    def request_url(self, port, url):
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", port)
        agent = ProxyAgent(ouinet_client_endpoint)
        return agent.request("GET", url)

    @inlineCallbacks
    def test_i2p_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the client --i2p--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        # #injector
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector(["--listen-on-i2p", "true"], i2pinjector_tunnel_ready)

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
            self.run_i2p_client(TestFixtures.I2P_CLIENT["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.I2P_CLIENT["port"]), "--injector-ep", injector_i2p_public_id, "http://localhost/"], i2pclient_tunnel_ready)
        
            #wait for the client tunnel to connect to the injector
            success = yield i2pclient_tunnel_ready

            content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
            for i in range(0,TestFixtures.MAX_NO_OF_TRIAL_I2P_REQUESTS):
                logging.debug("request attempt no " + str(i+1) + "...")
                defered_response = yield  self.request_echo(TestFixtures.I2P_CLIENT["port"], content)
                if defered_response.code == 200:
                    self.assertEquals(defered_response.code, 200)

                    response_body = yield readBody(defered_response)
                    self.assertEquals(response_body, content)
                    test_passed = True
                    break
                else:
                    logging.debug("request attempt no " + str(i+1) + " failed. with code " + str(defered_response.code))
                    yield task.deferLater(reactor, TestFixtures.I2P_TUNNEL_HEALING_PERIOD, lambda: None)

            if test_passed:
                break;
            else:
                #stop the i2p client so we can start a new one
                i2pclient = self.proc_list.pop()
                yield i2pclient.proc_end

        self.assertTrue(test_passed)

    @inlineCallbacks
    def test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        #injector
        injector_tcp_port_ready = defer.Deferred()
        self.run_tcp_injector(["--listen-on-i2p", "false", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)], injector_tcp_port_ready)

        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #client
        client_tcp_port_ready = defer.Deferred()
        self.run_tcp_client(TestFixtures.TCP_CLIENT["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.TCP_CLIENT["port"]), "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT), "http://localhost/"], client_tcp_port_ready)

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the client to open the port
        success = yield client_tcp_port_ready

        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield  self.request_echo(TestFixtures.TCP_CLIENT["port"], content)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, content)

    @inlineCallbacks
    def test_ipfs_cache(self):
        """
        Starts an echoing http server, a injector and a two clients and client1 send a unique http 
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The test waits for the response to be cached. 
        Then the second client request the same request makes sure that
        the response is served from cache.
        """
        #injector
        injector_tcp_port_ready = defer.Deferred()
        result_got_cached = defer.Deferred()
        ipfs_cache_injector = self.run_cache_injector(["--listen-on-i2p", "false", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)], injector_tcp_port_ready, result_got_cached)
        
        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #TODO: we are assuming that IPNS DB is announced before opening the port.
        # remove the is assumption.
        IPNS_end_point = ipfs_cache_injector.get_IPNS_ID()
        assert(len(IPNS_end_point) > 0);
        
        print "IPNS end point is: " + IPNS_end_point

        #tcp client
        client_tcp_port_ready = defer.Deferred()
        self.run_tcp_client(TestFixtures.CACHE_CLIENT[0]["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.CACHE_CLIENT[0]["port"]), "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT), "http://localhost/"], client_tcp_port_ready)

        #http_server
        self.test_http_server = self.run_http_server(
            TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the client to open the port
        success = yield defer.gatherResults([client_tcp_port_ready])
        self.assertTrue(success)

        page_url = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield self.request_page(TestFixtures.CACHE_CLIENT[0]["port"], page_url)
        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, TestFixtures.TEST_PAGE_BODY)
        
        # now waiting or injector to annouce caching the request
        success = yield result_got_cached
        self.assertTrue(success)

        #start cache client which supposed to read the response from cache
        client_cache_ready = defer.Deferred()
        cache_client = self.run_cache_client(TestFixtures.CACHE_CLIENT[1]["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.CACHE_CLIENT[1]["port"]), "--injector-ipns", IPNS_end_point, "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT), "http://localhost/"], client_cache_ready)

        import time

        # make sure that the client2 is ready to access the cache
        success = yield client_cache_ready
        IPNS_resoultion_done_time_stamp = time.time()

        self.assertTrue(success)
        self.assertTrue(cache_client.IPNS_resolution_start_time() > 0)

        logging.debug("IPNS resolution took: " + str(
            IPNS_resoultion_done_time_stamp -
            cache_client.IPNS_resolution_start_time()) + " seconds")

        # now request the same page from second client
        defered_response = yield self.request_page(
            TestFixtures.CACHE_CLIENT[1]["port"], page_url)

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
