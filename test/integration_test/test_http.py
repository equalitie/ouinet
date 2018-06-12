# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for Ouinet - test for http communication offered through different transports and caches
import os
import os.path

from twisted.internet import reactor, defer
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.protocol import Protocol
from twisted.web.client import ProxyAgent, Agent, readBody
from twisted.web.http_headers import Headers
from twisted.internet.defer import inlineCallbacks, Deferred

from twisted.trial.unittest import TestCase
from twisted.internet.base import DelayedCall

import urllib

#making random requests not to relying on cache
import string
import random

from ouinet_process_controler import OuinetConfig, OuinetClient, OuinetInjector, OuinetI2PInjector
from test_fixtures import TestFixtures
from test_http_server import TestHttpServer

import sys
import logging
import pdb

class OuinetTests(TestCase):
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        logging.basicConfig(stream=sys.stdout, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=TestFixtures.LOGGING_LEVEL, )

        #you can't set the timout in the test method itself :-(
        self.timeout = TestFixtures.TEST_TIMEOUT[self._testMethodName]
        logging.debug("setting timeout for " + self._testMethodName + " at " + str(TestFixtures.TEST_TIMEOUT[self._testMethodName]))

        self.proc_list = [] #keep track of all process we start for clean tear down

    def safe_random_str(self, length):
        letters = string.ascii_lowercase                             
        return ''.join(random.choice(letters) for i in range(length))
    
    def run_tcp_injector(self, injector_args, deffered_tcp_port_ready):
        injector = OuinetInjector(OuinetConfig(app_name=TestFixtures.TCP_INJECTOR_NAME, timeout=TestFixtures.TCP_TRANSPORT_TIMEOUT, argv=injector_args, ready_benchmark_regex=TestFixtures.TCP_PORT_READY_REGEX), deffered_tcp_port_ready)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_i2p_injector(self, injector_args, deferred_i2p_ready):
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME, TestFixtures.I2P_TRANSPORT_TIMEOUT, injector_args, ready_benchmark_regex=TestFixtures.I2P_TUNNEL_READY_REGEX), deferred_i2p_ready)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_tcp_client(self, name, args, deffered_tcp_port_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.TCP_TRANSPORT_TIMEOUT, args, ready_benchmark_regex=TestFixtures.TCP_PORT_READY_REGEX), deffered_tcp_port_ready)
        client.start()
        self.proc_list.append(client)

        return client

    def run_i2p_client(self, name, args, deferred_i2p_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_TRANSPORT_TIMEOUT, args, ready_benchmark_regex=TestFixtures.I2P_TUNNEL_READY_REGEX), deferred_i2p_ready)
        client.start()
        self.proc_list.append(client)

        return client

    def run_http_server(self, port):
        return TestHttpServer(port)

    def request_echo(self, content):
        """
        Send a get request to request the test server to echo the content 
        """
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", TestFixtures.TCP_CLIENT["port"])
        agent = ProxyAgent(ouinet_client_endpoint)
        return agent.request(
            "GET",
            "http://127.0.0.1:"+str(TestFixtures.TEST_HTTP_SERVER_PORT)+"/?content="+content)

    @inlineCallbacks
    def test_i2p_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the client --i2p--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        self.timeout = TestFixtures.I2P_TRANSPORT_TIMEOUT

        #injector
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector(["--listen-on-i2p", "true"], i2pinjector_tunnel_ready)

        #wait for the injector tunnel to be advertised
        success = yield i2pinjector_tunnel_ready## self.wait_for_i2p_tunnel_to_get_connected(i2pinjector_tunnel_ready)

        #client
        i2pclient_tunnel_ready = defer.Deferred()
        i2pclient = self.run_i2p_client(TestFixtures.I2P_CLIENT["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.I2P_CLIENT["port"]), "--injector-ipns", TestFixtures.INJECTOR_IPNS_PERSISTANT_IDENTITY["Identity"]["PeerID"], "--injector-ep", TestFixtures.INJECTOR_I2P_PUBLIC_ID, "http://localhost/"], i2pclient_tunnel_ready)

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)
        
        #wait for the client tunnel to connect to the injector
        success = yield i2pclient_tunnel_ready

        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        for i in range(1,TestFixtures.MAX_NO_OF_TRIAL_I2P_REQUESTS):
            defered_response = yield  self.request_echo(content)
            if defered_response.code == 200:
                break

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, content)

    @inlineCallbacks
    def test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the g client --tcp--> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        self.timeout = TestFixtures.TCP_TRANSPORT_TIMEOUT

        #injector
        injector_tcp_port_ready = defer.Deferred()
        self.run_tcp_injector(["--listen-on-i2p", "false", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT)], injector_tcp_port_ready)

        #wait for the injector to open the port
        success = yield injector_tcp_port_ready

        #client
        client_tcp_port_ready = defer.Deferred()
        self.run_tcp_client(TestFixtures.TCP_CLIENT["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.TCP_CLIENT["port"]), "--injector-ipns", TestFixtures.INJECTOR_IPNS_PERSISTANT_IDENTITY["Identity"]["PeerID"], "--injector-ep", "127.0.0.1:" + str(TestFixtures.TCP_INJECTOR_PORT), "http://localhost/"], client_tcp_port_ready)

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the client to open the port
        success = yield client_tcp_port_ready

        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        defered_response = yield  self.request_echo(content)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, content)

    def tearDown(self):
        deferred_procs = [] 
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()

        if hasattr(self,'test_http_server'):
            deferred_procs.append(self.test_http_server.stopListening())
            
        return defer.gatherResults(deferred_procs) 
    
class BeginningPrinter(Protocol):
    def __init__(self, finished):
        self.finished = finished
        self.remaining = 1024 * 10

    def dataReceived(self, bytes):
        if self.remaining:
            display = bytes[:self.remaining]
            print('Some data received:')
            print(display)
            self.remaining -= len(display)

    def connectionLost(self, reason):
        print('Finished receiving body:', reason.getErrorMessage())
        self.finished.callback(None)
