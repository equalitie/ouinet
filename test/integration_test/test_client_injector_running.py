import unittest
import pdb

from itestlib import OuinetClient, OuinetInjector
from test_fixtures import TestFixtures


class OuinetBasicSanityTest(unittest.TestCase):
    def test_run_client(self):
        ouinet_client_process = OuinetClient(TestFixtures.FIRST_CLIENT["name"],
                ( "--listen-on-tcp", "127.0.0.1:"+TestFixtures.FIRST_CLIENT["port"]
                , "--injector-ipns", TestFixtures.INJECTOR_IPNS_PERSISTANT_IDENTITY["Identity"]["PeerID"]
                , "--injector-ep", "127.0.0.1:" + str(TestFixtures.INJECTOR_PORT)
                , "http://localhost/"))

    def test_run_injector(self):
        pass

if __name__ == '__main__':
    from unittest import main
    unittest.main()
