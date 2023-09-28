import unittest

from itestlib import OuinetClient, OuinetInjector
from test_fixtures import TestFixtures


class OuinetBasicSanityTest(unittest.TestCase):
    def test_run_client(self):
        ouinet_client_process = OuinetClient(
            TestFixtures.OUINET_CLIENT["name"],
            "",
            "ouinet-client.conf",
            ("--listen-on-tcp", "127.0.0.1:" + TestFixtures.OUINET_CLIENT["port"],
             "--injector-ep", "127.0.0.1:" + str(TestFixtures.OUINET_INJECTOR["port"]),
             "http://localhost/"),
            30)

    def test_run_injector(self):
        pass


if __name__ == '__main__':
    from unittest import main
    unittest.main()
