import unittest

from itestlib import OuinetClient, OuinetInjector
from test_fixtures import TestFixtures


class OuinetBasicSanityTest(unittest.TestCase):
    def test_run_client(self):
        config_file_content = "\n".join((
            "enable-log-file = true",
            "cache-type = none",
            "listen-on-tcp = 0.0.0.0:9077",
            "front-end-ep = 0.0.0.0:9078",
        ))
        ouinet_client_process = OuinetClient(
            TestFixtures.OUINET_CLIENT["name"],
            "ouinet-basic",
            "ouinet-client.conf",
            config_file_content,
            30)

    def test_run_injector(self):
        pass


if __name__ == '__main__':
    from unittest import main
    unittest.main()
