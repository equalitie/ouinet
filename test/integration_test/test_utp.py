# Integration tests for Ouinet - test for UDP uTP communication

import json
import logging
import sys

from twisted.internet import reactor, defer
from twisted.web.client import Agent, readBody
from twisted.internet.defer import inlineCallbacks

from twisted.trial.unittest import TestCase

from ouinet_process_controler import OuinetConfig
from ouinet_process_controler import OuinetClient
from test_fixtures import TestFixtures


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

        # you can't set the timeout in the test method itself :-(
        self.timeout = TestFixtures.TCP_TRANSPORT_TIMEOUT
        logging.debug(
            "setting timeout for "
            + self._testMethodName
            + " at "
            + str(TestFixtures.TCP_TRANSPORT_TIMEOUT)
        )

        self.proc_list = []  # keep track of all process we start for clean tear down

    def run_tcp_client(self, name, idx_key, args, deffered_tcp_port_ready):
        config = OuinetConfig(
            name,
            TestFixtures.TCP_TRANSPORT_TIMEOUT,
            args,
            benchmark_regexes=[TestFixtures.TCP_PORT_READY_REGEX],
        )
        client = OuinetClient(config, [deffered_tcp_port_ready])
        client.start()
        self.proc_list.append(client)
        return client

    def deferred_request_api_status(self, endpoint):
        """
        Send a get request to the Ouinet API status
        """
        url = "http://%s/api/status" % endpoint
        agent = Agent(reactor)
        return agent.request(b"GET", url.encode())

    @inlineCallbacks
    def test_utp_endpoint_announcement(self):
        """
        Starts a Ouinet client with the default uTP port, sets a new uTP port
        and restarts the service.

        The UDP uTP endpoint announced to the DHT should be the same one that was
        set by the command line interface or the config file.
        """
        logging.debug("################################################")
        logging.debug("test_utp_endpoint_announcement")
        logging.debug("################################################")

        # client
        client_tcp_port_ready = defer.Deferred()

        # use BEP5 cache mechanism
        args = [
            "--disable-origin-access",
            "--cache-type=bep5-http",
            "--cache-http-public-key",
            "zh6ylt6dghu6swhhje2j66icmjnonv53tstxxvj6acu64sc62fnq",
            "--listen-on-tcp",
            "127.0.0.1:" + str(TestFixtures.TCP_CLIENT["port"]),
        ]
        self.run_tcp_client(
            TestFixtures.TCP_CLIENT["name"], None, args, client_tcp_port_ready
        )

        # wait for the client to open the port
        yield client_tcp_port_ready

        url = "http://127.0.0.1:8078/api/status"
        agent = Agent(reactor)
        deferred_api_status = yield agent.request(b"GET", url.encode())
        print(deferred_api_status)
        response_body = yield readBody(deferred_api_status)
        api_status = json.loads(response_body)
        udp_port = api_status["local_udp_endpoints"][0].split(":")[1]
        self.assertEquals("28729", udp_port)

    def tearDown(self):
        deferred_procs = []
        for cur_proc in self.proc_list:
            deferred_procs.append(cur_proc.proc_end)
            cur_proc.stop()

        if hasattr(self, "test_http_server"):
            deferred_procs.append(self.test_http_server.stopListening())

        return defer.gatherResults(deferred_procs)
