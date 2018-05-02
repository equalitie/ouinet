# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for Ouinet
#
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

from itestlib import OuinetClient, OuinetInjector
from test_fixtures import TestFixtures
from test_http_server import TestHttpServer

import pdb

class OuinetTests(TestCase):
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        self.proc_list = [] #keep track of all process we start for clean tear down

    def safe_random_str(self, length):
        letters = string.ascii_lowercase                             
        return ''.join(random.choice(letters) for i in range(length))
    
    def run_client(self, name, args):
        client = OuinetClient(name, TestFixtures.TIMEOUT_LEN, args)
        self.proc_list.append(client)

        return client

    def run_injector(self, injector_args):
        #we first need to run our http server
        injector = OuinetInjector("injector", TestFixtures.TIMEOUT_LEN, injector_args)
        self.proc_list.append(injector)

        return injector

    def run_http_server(self, port):
        return TestHttpServer(port)

    @inlineCallbacks
    def request_echo(self, content):
        """
        Send a get request to request the test server to echo the content 
        """
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "127.0.0.1", TestFixtures.FIRST_CLIENT["port"])
        agent = ProxyAgent(ouinet_client_endpoint)
        agent = Agent(reactor)
        TestFixtures.TEST_HTTP_SERVER_PORT = 8080
        defered_response = yield agent.request(
            "GET",
            "http://127.0.0.1:"+str(TestFixtures.TEST_HTTP_SERVER_PORT)+"/?content="+content)

        self.assertEquals(defered_response.code, 200)

        response_body = yield readBody(defered_response)
        self.assertEquals(response_body, content)

    def wait_for_port_to_get_open(self, server, port, timeout=None):
        """ Wait for network service to appear 
            @param timeout: in seconds, if None or 0 wait forever
            @return: True of False, if timeout is None may return only True or
            throw unhandled network exception
        """
        import socket
        import errno

        s = socket.socket()
        if timeout:
            from time import time as now
            # time module is needed to calc timeout shared between two exceptions
            end = now() + timeout

        while True:
            try:
                if timeout:
                    next_timeout = end - now()
                    if next_timeout < 0:
                        return False
                    else:
                        s.settimeout(next_timeout)

                s.connect((server, port))

            except socket.timeout, err:
                # this exception occurs only if timeout is set
                if timeout:
                    return False

            except socket.error, err:
                # catch timeout exception from underlying network library
                # this one is different from socket.timeout
                # we need to capture and ignore 111 connection refused
                # because it also says that the app has not opened the
                # socket yet
                if type(err.args) != tuple or (err[0] != errno.ETIMEDOUT and err[0] != errno.ECONNREFUSED):
                    raise
            else:
                s.close()
                return True

    def test_tcp_transport(self):
        """
        Starts an echoing http server, a injector and a client and send a unique http 
        request to the echoing http server through the client -> injector -> http server
        and make sure it gets the correct echo. The unique request makes sure that
        the response is from the http server and is not cached.
        """
        #injector
        self.run_injector(["--listen-on-i2p", "true", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.INJECTOR_PORT)])

        #client
        self.run_client(TestFixtures.FIRST_CLIENT["name"], ["--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.FIRST_CLIENT["port"]), "--injector-ipns", TestFixtures.INJECTOR_IPNS_PERSISTANT_IDENTITY["Identity"]["PeerID"], "--injector-ep", "127.0.0.1:" + str(TestFixtures.INJECTOR_PORT), "http://localhost/"])

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #wait for the injector to open the port
        self.wait_for_port_to_get_open("127.0.0.1", TestFixtures.INJECTOR_PORT);

        #wait for the client to open the port
        self.wait_for_port_to_get_open("127.0.0.1", TestFixtures.FIRST_CLIENT["port"]);

        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        self.request_echo(content)

    def tearDown(self):
        deferred_procs = [] 
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()
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
