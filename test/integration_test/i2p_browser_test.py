import logging
import sys

from twisted.internet import reactor, defer, task
from twisted.internet.protocol import ProcessProtocol

from test_fixtures import TestFixtures
from ouinet_process_controler import OuinetInjector, OuinetI2PInjector, OuinetClient, OuinetIPFSCacheInjector, OuinetBEP5CacheInjector, OuinetConfig

import pdb
class I2PProcessProtocoltest():
    def test_me(self):
        print("test me")
        pdb.set_trace()
        
class I2PProcessProtocol():#(ProcessProtocol):
    def __init__(self):#, deferred):
        #self.deferred = deferred
        self.proc_list = []

    #def processEnded(self, reason):
    #    self.deferred.callback(reason)

    def run_i2p_injector(self, injector_args, deferred_i2p_ready):
        injector = OuinetI2PInjector(OuinetConfig(TestFixtures.I2P_INJECTOR_NAME + "_i2p", TestFixtures.I2P_BROWSER_TEST_TIMEOUT, injector_args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready],  TestFixtures.I2P_INJECTOR_PRIVATE_KEY)
        injector.start()
        self.proc_list.append(injector)

        return injector

    def run_i2p_client(self, name, idx_key, args, deferred_i2p_ready):
        client = OuinetClient(OuinetConfig(name, TestFixtures.I2P_BROWSER_TEST_TIMEOUT, args, benchmark_regexes=[TestFixtures.I2P_TUNNEL_READY_REGEX]), [deferred_i2p_ready])
        client.start()
        self.proc_list.append(client)

    def delayed_callback(self):
        print("This is executed after 5 seconds!")
        #reactor.stop()  # Stop the reactor after the callback is executed

    def wait_for_seconds(seconds):
        d = defer.Deferred()
        reactor.callLater(seconds, d.callback, None)
        return d

    @defer.inlineCallbacks
    def test_yield(self):
          print("Waiting for 5 seconds...")
          yield wait_for_seconds(5)
          self.delayed_callback()

    @defer.inlineCallbacks
    def setup_i2p_injector_and_client(self):
        """
        Starts an injector and a client and wait indefinitely expecting the user to
        use a browser to test the connection
        """
        #pdb.set_trace()
        logging.basicConfig(stream=sys.stderr, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=TestFixtures.LOGGING_LEVEL, )

        self.timeout = TestFixtures.TEST_TIMEOUT["i2p_browser_test"]
        # logging.debug("setting timeout for " + self._testMethodName + " at " + str(TestFixtures.TEST_TIMEOUT[self._testMethodName]))
        logging.debug("################################################")
        logging.debug("i2p browser test")
        logging.debug("################################################")
    
        #injector
        i2pinjector_tunnel_ready = defer.Deferred()
        i2pinjector = self.run_i2p_injector(["--listen-on-i2p", "true", "--log-level", "DEBUG",
                                               ], i2pinjector_tunnel_ready) #"--disable-cache"
        self.proc_list.append(i2pinjector)

        #wait for the injector tunnel to be advertised
        print("Waiting for injector tunnel to get established...")

        success = yield i2pinjector_tunnel_ready
        print("Tunnel has stablished...")
 
        #we only can request that after injector is ready
        injector_i2p_public_id = i2pinjector.get_I2P_public_ID()

        #wait so the injector id gets advertised on the DHT
        logging.debug("waiting " + str(TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD) + " secs for the tunnel to get advertised on the DHT...")
        yield task.deferLater(reactor, TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD, lambda: None)

        print("Waited for %i seconds..."%TestFixtures.I2P_DHT_ADVERTIZE_WAIT_PERIOD)
        
        #client
        print("Starting client...")
        i2pclient_tunnel_ready = defer.Deferred()

        # #use only Proxy or Injector mechanisms
        i2p_client = self.run_i2p_client( TestFixtures.I2P_CLIENT["name"], None
                                           , [ "--disable-origin-access", "--disable-cache"
                                               , "--listen-on-tcp", "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"])
                                               , "--injector-ep", "i2p:" + injector_i2p_public_id,
                                               "--log-level", "DEBUG",
                                              ]
                                           , i2pclient_tunnel_ready)

        print("client is getting ready")
        # self.proc_lit.append(i2p_client)
        success = yield i2pclient_tunnel_ready


def run_process():
    deferred = defer.Deferred()
    protocol = MyProcessProtocol(deferred)
    reactor.spawnProcess(protocol, "python", ["python", "-c", "print('Hello from child process')"])
    return deferred

def delayed_callback():
    print("This is executed after 5 seconds!")
    reactor.stop()  # Stop the reactor after the callback is executed

def wait_for_seconds(seconds):
    d = defer.Deferred()
    reactor.callLater(seconds, d.callback, None)
    return d

# Main function
@defer.inlineCallbacks
def main_2():
    print("Waiting for 5 seconds...")
    yield wait_for_seconds(5)
    delayed_callback()
# def main():
#     d = run_process()
#     d.addCallback(lambda _: print("Process finished"))
#     d.addBoth(lambda _: reactor.stop())  # Stop the reactor after process ends
if __name__ == "__main__":
    #deferred = defer.Deferred()
    i2p_process_protocol = I2PProcessProtocol()#deferred)
    #deferred.addCallback(lambda _: print("Process finished"))
    #deferred.addBoth(lambda _: reactor.stop())  # Stop the re
    #pdb.set_trace()
    #reactor.callWhenRunning(i2p_process_protocol.setup_i2p_injector_and_client)
    i2p_process_protocol.setup_i2p_injector_and_client()
    # pdb.set_trace()
    
    # pdb.set_trace()
    #i2p_process_protocol.test_yield()
    test = I2PProcessProtocoltest()
    reactor.run()
    test.test_me()
