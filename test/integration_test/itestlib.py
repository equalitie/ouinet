# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - library routines.

import difflib
import errno
import os
import re
import shlex
import socket
import subprocess
import threading
import time

import pdb

from test_fixtures import TestFixtures

from twisted.internet import protocol, reactor, defer

ouinet_env = {}
ouinet_env.update(os.environ)

class OuinetProcess:
    def make_config_folder(self):
        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME)

        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name)

        self.config_folder = TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name

    def setup_config(self, app_name, timeout, config_file_name, config_file_content):
        #making the necessary folders for the configs and cache
        self.app_name = app_name
        self.timeout = timeout
        self.make_config_folder()

        #we need to make a minimal configuration file
        #we overwrite any existing config file to make
        #the test canonical (also trial delete its temp
        #folder each time
        with open(self.config_folder + "/" + config_file_name, "w") as conf_file:
            conf_file.write(config_file_content)

    def start(self, argv):
        #do not start twice
        if hasattr(self, '_has_started') and self._has_started:
            return

        self._proc_protocol = OuinetProcessProtocol()
        self._proc = reactor.spawnProcess(self._proc_protocol, argv[0], argv)

        self._has_started = True

        #we add a twisted timer to kill the process after timeout
        self.timeout_killer = reactor.callLater(self.timeout, self.stop)

        #we *might* need to make sure that the process has ended before
        #ending the test because Twisted Trial might get mad otherwise
        self.proc_end = defer.Deferred()
        self._proc_protocol.onExit = self.proc_end

    def stop(self):
        if hasattr(self, '_has_started') and self._has_started: #stop only if started
            self.timeout_killer.cancel()
            self._proc_protocol.transport.loseConnection()
            self._proc.signalProcess("TERM")
            self._has_started = False

class OuinetProcessProtocol(protocol.ProcessProtocol):
    """
    Protocols are the way to communicate with different players
    in a system in Twisted. This protocol is receiving outputs
    from ouinet client/injector process and report failure 
    in case of fatal error
    """
    def errReceived(self, data):
        """
        listen for fatal errors and abort the test
        """
        if (re.match(r'\[ABORT\]', data)):
            raise Exception, "Fatal error"

    def processExited(self, reason):
        self.onExit.callback(self)

class OuinetClient(OuinetProcess):
    def __init__(self, client_name, timeout, args):
        #making the necessary folders for the configs and cache
        self.setup_config(client_name, timeout, "ouinet-client.conf", TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT)

        argv = [ouinet_env['OUINET_BUILD_DIR'] + "client", "--repo", self.config_folder]
        argv.extend(args)

        self.start(argv)

# As above, but for the 'injector' 
class OuinetInjector(OuinetProcess):
    def __init__(self, injector_name, timeout, args):
        self.setup_config(injector_name, timeout, "ouinet-injector.conf", TestFixtures.INJECTOR_CONF_FILE_CONTENT)
        argv = [ouinet_env['OUINET_BUILD_DIR'] + "injector", "--repo", self.config_folder]
        argv.extend(args)

        self.start(argv)

