# Copyright 2018, 2019 eQualit.ie, Inc.
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
from ouinet_process_controler import (
    OuinetInjector, OuinetI2PInjector, OuinetIPFSCacheInjector, OuinetBEP44CacheInjector)
from ouinet_process_controler import (
    OuinetClient, OuinetIPFSClient, OuinetBEP44Client)
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
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME + "_i2p", TestFixtures.I2P_TRANSPORT_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready])
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_ipfs_injector(self, injector_args, deferred_tcp_port_ready, deferred_result_got_cached):
        config = self._cache_injector_config(TestFixtures.IPFS_CACHE_TIMEOUT,
                                             ["--default-index", "btree"] + injector_args)
        return self._run_cache_injector(
            OuinetIPFSCacheInjector, config,
            deferred_tcp_port_ready, deferred_result_got_cached)

    def run_bep44_injector(self, injector_args, deferred_tcp_port_ready, deferred_result_got_cached):
        config = self._cache_injector_config(TestFixtures.BEP44_CACHE_TIMEOUT,
                                             ["--default-index", "bep44"] + injector_args)
        return self._run_cache_injector(
            OuinetBEP44CacheInjector, config,
            deferred_tcp_port_ready, deferred_result_got_cached)

    def _cache_injector_config(self, timeout, args):
        return OuinetConfig(TestFixtures.CACHE_INJECTOR_NAME, timeout, args,
                            benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX,
                                               TestFixtures.REQUEST_CACHED_REGEX])

    def _run_cache_injector(self, proc_class, config, deferred_tcp_port_ready, deferred_result_got_cached):
        injector = proc_class(config, [deferred_tcp_port_ready, deferred_result_got_cached])
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

    def run_ipfs_client(self, name, idx_key, args, deferred_cache_ready):
        config = OuinetConfig(name, TestFixtures.IPFS_CACHE_TIMEOUT,
                              ["--default-index", "btree", "--injector-ipns", idx_key] + args,
                              benchmark_regexes=[TestFixtures.IPFS_CACHE_READY_REGEX])
        return self._run_cache_client(OuinetIPFSClient, config, deferred_cache_ready)

    def run_bep44_client(self, name, idx_key, args, deferred_cache_ready):
        config = OuinetConfig(name, TestFixtures.BEP44_CACHE_TIMEOUT,
                              ["--default-index", "bep44", "--bittorrent-public-key", idx_key] + args,
                              benchmark_regexes=[TestFixtures.BEP44_CACHE_READY_REGEX])
        return self._run_cache_client(OuinetBEP44Client, config, deferred_cache_ready)

    def _run_cache_client(self, proc_class, config, deferred_cache_ready):
        client = proc_class(config, [deferred_cache_ready])
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
        logging.debug("################################################")
        logging.debug("test_i2p_transport");
        logging.debug("################################################")
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

            #use only Proxy or Injector mechanisms
            self.run_i2p_client( TestFixtures.I2P_CLIENT["name"]
                               , [ "--disable-origin-access", "--disable-cache"
                                 , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                 , "--injector-ep", injector_i2p_public_id
                                 , "http://localhost/"]
                               , i2pclient_tunnel_ready)
        
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
        logging.debug("################################################")
        logging.debug("test_tcp_transport");
        logging.debug("################################################")
        #injector
        injector_tcp_port_ready = defer.Deferred()
        self.run_tcp_injector(["--listen-on-i2p", "false", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)], injector_tcp_port_ready)

        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #client
        client_tcp_port_ready = defer.Deferred()

        #use only Proxy or Injector mechanisms
        self.run_tcp_client( TestFixtures.TCP_CLIENT["name"]
                           , [ "--disable-origin-access", "--disable-cache"
                             , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_CLIENT["port"])
                             , "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)
                             , "http://localhost/"]
                           , client_tcp_port_ready)

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
        logging.debug("################################################")
        logging.debug("test_ipfs_cache");
        logging.debug("################################################")
        return self._test_cache(self.run_ipfs_injector, self.run_ipfs_client)

    @inlineCallbacks
    def test_bep44_cache(self):
        logging.debug("################################################")
        logging.debug("test_bep44_cache");
        logging.debug("################################################")
        return self._test_cache(self.run_bep44_injector, self.run_bep44_client)

    def _test_cache(self, run_cache_injector, run_cache_client):
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
        cache_injector = run_cache_injector(["--listen-on-i2p", "false", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)], injector_tcp_port_ready, result_got_cached)
        
        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #TODO: we are assuming that the index key is announced before opening the port.
        # remove the is assumption.
        index_key = cache_injector.get_index_key()
        assert(len(index_key) > 0);
        
        print "Index key is: " + index_key

        #tcp client, use only Injector mechanism
        client_tcp_port_ready = defer.Deferred()
        self.run_tcp_client( TestFixtures.CACHE_CLIENT[0]["name"]
                           , [ "--disable-origin-access", "--disable-proxy-access", "--disable-cache"
                             , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[0]["port"])
                             , "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)
                             , "http://localhost/"]
                           , client_tcp_port_ready)

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

        #start cache client which supposed to read the response from cache, use only Cache mechanism
        client_cache_ready = defer.Deferred()
        cache_client = run_cache_client(
            TestFixtures.CACHE_CLIENT[1]["name"], index_key,
            [ "--disable-origin-access", "--disable-proxy-access"
            , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.CACHE_CLIENT[1]["port"])
            , "http://localhost/"],
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
