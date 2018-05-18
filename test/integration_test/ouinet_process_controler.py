# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire
# ouinet client and injectors for different tests situation

import errno
import os
import threading
import time

import pdb

from test_fixtures import TestFixtures

from twisted.internet import reactor, defer

from ouinet_process_protocol import OuinetProcessProtocol, OuinetI2PEnabledProcessProtocol

ouinet_env = {}
ouinet_env.update(os.environ)


class OuinetConfig(object):
    """
    a class contains all configuration corresponding
    to a Ouinet (client/injector) process. This is
    to make it easier to send the config to different
    process
    """

    def __init__(self, app_name = "generic ouinet app",
                 timeout = TestFixtures.DEFAULT_PROCESS_TIMEOUT, argv = [],
                 config_file_name = "ouinet.conf", 
                 config_file_content = ""):
        """
        Initials a config object which is used to properly run a ouinet process
        Args
        timeout: the timeout for the lifetime of the process in  seconds, be
                 used to kill the process if it doesn't finish its tasks by
                 that timeout
        app_name: a name for the process uses for naming the config folder to
                  preventing config overlapping

        config_file_content: the content of config file to be written as text
        """
        self.app_name = app_name
        self.config_file_name = config_file_name
        self.config_file_content = config_file_content
        self.timeout = timeout
        self.argv = argv

class OuinetProcess(object):
    def __init__(self, ouinet_config = OuinetConfig()):
        """
        perform the initialization tasks common between all clients 
        and injectors:
        - makes a basic config folder and file 
        - sets the timeout for the process 
 
        Args 
        ouinet_config A OuinetConfig instance containing the configuration
                      related to this process
        """
        self.config = ouinet_config
        self._proc_protocol = OuinetProcessProtocol() # default protocol
        # in case the communication process protocol is not explicitly set 
        # starts a default process protocol to check on Fatal errors
        self._has_started = False
        self.setup_config()

    def setup_config(self):
        """
        setups various configs, write a fresh config file and make
        the process protocol object
        """
        # making the necessary folders for the configs and cache
        self.make_config_folder()

        # we need to make a minimal configuration file
        # we overwrite any existing config file to make
        # the test canonical (also trial delete its temp
        # folder each time
        with open(self.config.config_folder_name + "/" + self.config.config_file_name,
                  "w") as conf_file:
            conf_file.write(self.config.config_file_content)

    def make_config_folder(self):
        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME)

        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME + "/" +
                              self.config.app_name):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME + "/" +
                        self.config.app_name)

        self.config.config_folder_name = TestFixtures.REPO_FOLDER_NAME + "/" + self.config.app_name
                             

    def set_process_protocol(self, process_protocol):
        self._proc_protocol = process_protocol

    def start(self):
        """
        starts the actual process using twisted spawnProcess

        Args
          argv: command line arguments where argv[0] should be the name of the
                executable program with its code
          process_protoco: twisted process protocol which deals with processing 
                           the output of the ouinet process. If None, a generic
                           Ouinet Process
        """
        # do not start twice
        if self._has_started:
            return

        self._proc_protocol.app_name = self.config.app_name

        self._proc = reactor.spawnProcess(self._proc_protocol,
                                          self.config.argv[0],
                                          self.config.argv)
        self._has_started = True

        # we add a twisted timer to kill the process after timeout
        print self.config.app_name, "has timeout", self.config.timeout
        self.timeout_killer = reactor.callLater(self.config.timeout, self.stop)

        # we *might* need to make sure that the process has ended before
        # ending the test because Twisted Trial might get mad otherwise
        self.proc_end = defer.Deferred()
        self._proc_protocol.onExit = self.proc_end

    def stop(self):
        if self._has_started: # stop only if started
            self.timeout_killer.cancel()
            self._proc_protocol.transport.loseConnection()
            self._proc.signalProcess("TERM")
            self._has_started = False

            
class OuinetClient(OuinetProcess):
    def __init__(self, client_config):
        client_config.config_file_name = "ouinet-client.conf"
        client_config.config_file_content = TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT
        super(OuinetClient, self).__init__(client_config)

        self.config.argv = [ouinet_env['OUINET_BUILD_DIR' ] + "/client",
                                "--repo",
                                self.config.config_folder_name] + self.config.argv
        

class OuinetI2PClient(OuinetClient):
    def __init__(self, client_config, i2p_ready):
        """
        Args
        i2p_ready is a deferred object whose callback is
                  called when the i2p tunnel to the injector gets
                  connected
        """
        super(OuinetI2PClient, self).__init__(client_config)

        # we need a process protocol which reacts on i2p related output
        self.set_process_protocol(OuinetI2PEnabledProcessProtocol())
        self._proc_protocol.set_i2p_is_ready_object(i2p_ready)

class OuinetInjector(OuinetProcess):
    """
    As above, but for the 'injector'
    Starts an injector process by passing the args to the service

    Args
    injector_name: the name of the injector which determines the config folder
                   name
    timeout: how long before killing the injector process
    args: list containing command line arguments passed directly to the injector
    i2p_ready: is a Deferred object whose callback is being called when i2p
              tunnel is ready
    """
    def __init__(self, injector_config):
        injector_config.config_file_name = TestFixtures.INJECTOR_CONF_FILE_NAME
        injector_config.config_file_content = \
          TestFixtures.INJECTOR_CONF_FILE_CONTENT
        super(OuinetInjector, self).__init__(injector_config)
        self.config.argv = [ouinet_env['OUINET_BUILD_DIR'] + "injector",
                            "--repo",
                            self.config.config_folder_name] + self.config.argv

class OuinetI2PInjector(OuinetInjector):
    """
    As above, but for the 'injector' 
    It is a child of OuinetI24injector with i2p ouiservice

    Args
    injector_name: the name of the injector which determines the config folder
                  name
    timeout: how long before killing the injector process
    args: list containing command line arguments passed directly to the 
    injector
    TODO: i2p_ready: is a Deferred object whose callback is being called when
                      i2p tunnel is ready
    """
    def __init__(self, injector_config, i2p_ready = None):
        super(OuinetI2PInjector, self).__init__(injector_config)
        self._setup_i2p_private_key()

    def _setup_i2p_private_key(self):
        if not os.path.exists(self.config.config_folder_name+"/i2p"):
            os.makedirs(self.config.config_folder_name+"/i2p")

        with open(self.config.config_folder_name+"/i2p/i2p-private-key", "w") \
          as private_key_file:
            private_key_file.write(TestFixtures.INJECTOR_I2P_PRIVATE_KEY)
