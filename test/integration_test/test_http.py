# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for Ouinet
#

import os
import os.path

from twisted.internet import reactor
from twisted.internet.endpoints import TCP4ClientEndpoint
from twisted.internet.protocol import Protocol
from twisted.web.client import ProxyAgent, Agent, readBody
from twisted.web.http_headers import Headers
from twisted.internet.defer import inlineCallbacks, Deferred

from twisted.trial.unittest import TestCase
from twisted.internet.base import DelayedCall

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

    def safe_random_str(self, length):
        letters = string.ascii_lowercase                             
        return ''.join(random.choice(letters) for i in range(length))
    
    def run_client(self, name, args):
        client =  OuinetClient(name, args)
        # errors = ""
        # try:
        #     testtl = client.check_completion(name + " tester")
        #     # if testtl != self.reftl:
        #     #     errors += diff("errors in transfer:", self.reftl, testtl)

        # except AssertionError, e:
        #     errors += e.message
        # except Exception, e:
        #     errors += repr(e)

        #errors += st.check_completion(label + " proxy", errors != "")

        #if errors != "":
        #    self.fail("\n" + errors)

        return client
        

    def run_injector(self, injector_args):
        #we first need to run our http server
        injector = OuinetInjector("injector", injector_args)

    def run_http_server(self, port):
        return TestHttpServer(port)

    @inlineCallbacks
    def request_echo(self, content):
        """
        Send a get request to request the test server to echo the content 
        """
        agent = Agent(reactor)
        print "our agent is ", agent
        defered_response = yield agent.request(
            "GET",
            "http://127.0.0.1:"+TestFixtures.FIRST_CLIENT["port"]+"/content="+content,
            Headers({'User-Agent': ['Ouinet Test Client']}),
            None)
        set_trace
        print defered_response.code
        print dir(defered_response)
        self.assertEquals(defered_response.code, 200)
        self.assertEquals(defered_response.body, content)

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
                if type(err.args) != tuple or err[0] != errno.ETIMEDOUT:
                    raise
            else:
                s.close()
                return True

    # def doProxyTest(self, label, proxy_args, st_args):
    #     """
    #     It runs a proxy with proxy_args and then call doTest

    #     INPUT:
    #        - ``label``: The test title
    #        - ``proxy_args``: arguments to be passed to the test proxy
    #        - ``st_args``: arguments to be passed to Stegotorus
    #     """
    #     test_proxy = TesterProxy(proxy_args)

    #     self.doTest(label, st_args)

    @inlineCallbacks
    def test_i2p_client(self):
        """
        All tests need an injector and a http server
        """
        #injector
        self.run_injector(("--listen-on-i2p", "true", "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.INJECTOR_PORT)))

        #http_server
        self.test_http_server = self.run_http_server(TestFixtures.TEST_HTTP_SERVER_PORT)

        #client
        self.run_client(TestFixtures.FIRST_CLIENT["name"], ("--listen-on-tcp", "127.0.0.1:"+str(TestFixtures.FIRST_CLIENT["port"]), "--injector-ipns", TestFixtures.INJECTOR_IPNS_PERSISTANT_IDENTITY["Identity"]["PeerID"], "--injector-ep", "127.0.0.1:" + str(TestFixtures.INJECTOR_PORT), "http://localhost/"))

        #wait for the client to open the port
        self.wait_for_port_to_get_open("127.0.0.1", int(TestFixtures.FIRST_CLIENT["port"]));
        
        content = self.safe_random_str(TestFixtures.RESPONSE_LENGTH)
        ouinet_client_endpoint = TCP4ClientEndpoint(reactor, "localhost", TestFixtures.FIRST_CLIENT["port"])
        agent = ProxyAgent(ouinet_client_endpoint)
        #agent = Agent(reactor)
        defered_response = yield agent.request(
            "GET",
            "http://127.0.0.1:"+str(TestFixtures.ToEST_HTTP_SERVER_PORT)+"/?content="+content,
            Headers({'User-Agent': ['Ouinet Test Client']}),
            None)

        print defered_response.code
        response_body = yield readBody(defered_response)
        print response_body

        pdb.set_trace()
        self.assertEquals(defered_response.code, 200)
        self.assertEquals(response_body, content)
        #self.request_echo(content)

    def tearDown(self):
        return self.test_http_server.stopListening()
    
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

# # Synthesize TimelineTest+TestCase subclasses for every 'tl_*' file in
# # the test directory.
# def load_tests(loader, standard_tests, pattern):
#     suite = TestSuite()
#     testdir = os.path.dirname(__file__)

#     testdir = (testdir == '') and '.' or testdir

#     for f in sorted(os.listdir(testdir)):
#         if f.startswith('tl_'):
#             script = os.path.join(testdir, f)
#             cls = type(f[3:],
#                        (TimelineTest, TestCase),
#                        { 'scriptFile': script })
#             suite.addTests(loader.loadTestsFromTestCase(cls))
#     return suite

# if __name__ == '__main__':
#     from unittest import main
#     main()
