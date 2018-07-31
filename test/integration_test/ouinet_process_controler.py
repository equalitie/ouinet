# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire
# ouinet client and injectors for different tests situation

import errno
import os
import time
import logging
import json

import pdb

from test_fixtures import TestFixtures

from twisted.internet import reactor, defer, task
from ouinet_process_protocol import OuinetProcessProtocol, OuinetIPFSCacheProcessProtocol

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
                 benchmark_regexes = [],
                 config_file_content = ""):
        """
        Initials a config object which is used to properly run a ouinet process
        Args
        timeout: the timeout for the lifetime of the process in  seconds, be
                 used to kill the process if it doesn't finish its tasks by
                 that timeout
        app_name: a name for the process uses for naming the config folder to
                  preventing config overlapping

        ready_benchmark_regex: is a string which the process will look into log to callback
                         the deferred object to annouce that the process is ready

        config_file_content: the content of config file to be written as text
        """
        self.app_name = app_name
        self.config_file_name = config_file_name
        self.config_file_content = config_file_content
        self.timeout = timeout
        self.argv = argv
        self.benchmark_regexes = benchmark_regexes

class OuinetProcess(object):
    def __init__(self, ouinet_config, deferred_events):
        """
        perform the initialization tasks common between all clients 
        and injectors:
        - makes a basic config folder and file 
        - sets the timeout for the process 
 
        Args 
        ouinet_config            A OuinetConfig instance containing the configuration
                                 related to this process
        process_ready_deferred   a deferred object which get called back when the process is ready
        """
        self.config = ouinet_config
        self._proc_protocol = OuinetProcessProtocol(proc_config = self.config, ready_benchmark_regex = ouinet_config.benchmark_regexes[TestFixtures.READY_REGEX_INDEX], ready_deferred=deferred_events[TestFixtures.READY_REGEX_INDEX]) # default protocol
        # in case the communication process protocol is not explicitly set 
        # starts a default process protocol to check on Fatal errors
        self._has_started = False
        self._term_signal_sent = False
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
        logging.debug(self.config.app_name + " times out in " + str(self.config.timeout) + " seconds")
        self.timeout_killer = reactor.callLater(self.config.timeout, self.stop)

        # send a pulse to keep the output of the proccess alive
        self.pulse_timer = task.LoopingCall(self._proc_protocol.pulse_out)
        self.pulse_timer.start(TestFixtures.KEEP_IO_ALIVE_PULSE_INTERVAL) # call every second

        # we *might* need to make sure that the process has ended before
        # ending the test because Twisted Trial might get mad otherwise
        self.proc_end = defer.Deferred()
        self._proc_protocol.onExit = self.proc_end

    def send_term_signal(self):
        if not self._term_signal_sent:
            logging.debug("Sending term signal to " + self.config.app_name)
            self._term_signal_sent = True
            self._proc.signalProcess("TERM")

    def stop(self):
        if self._has_started: # stop only if started
            self._has_started = False
            logging.debug("process " + self.config.app_name + " stopping")

            if self.timeout_killer.active():
                self.timeout_killer.cancel()

            self.pulse_timer.stop()

            self.send_term_signal()
            self._proc_protocol.transport.loseConnection()

class OuinetClient(OuinetProcess):
    def __init__(self, client_config, deferred_events):
        client_config.config_file_name = "ouinet-client.conf"
        client_config.config_file_content = TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT
        super(OuinetClient, self).__init__(client_config, deferred_events)

        self.config.argv = [ouinet_env['OUINET_BUILD_DIR' ] + "/client",
                                "--repo",
                                self.config.config_folder_name] + self.config.argv

class OuinetIPFSClient(OuinetClient):
    def __init__(self, client_config, deferred_events):
        super(OuinetIPFSClient, self).__init__(client_config, deferred_events)

        self.set_process_protocol(OuinetIPFSCacheProcessProtocol(proc_config = self.config, benchmark_regexes = client_config.benchmark_regexes, benchmark_deferreds=deferred_events))

    def served_from_cache(self):
        """
        returns true if any request has been served from cache

        """
        if (self._proc_protocol):
            return self._proc_protocol.served_from_cache()

        return False

    def IPNS_resolution_start_time(self):
        if (self._proc_protocol):
            return self._proc_protocol.IPNS_resolution_start_time

        return 0
        
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
    def __init__(self, injector_config, deferred_events):
        injector_config.config_file_name = TestFixtures.INJECTOR_CONF_FILE_NAME
        injector_config.config_file_content = \
          TestFixtures.INJECTOR_CONF_FILE_CONTENT
        super(OuinetInjector, self).__init__(injector_config, deferred_events)
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
    def __init__(self, injector_config, deferred_events):
        super(OuinetI2PInjector, self).__init__(injector_config, deferred_events)
        self._setup_i2p_private_key()

    def _setup_i2p_private_key(self):
        if not os.path.exists(self.config.config_folder_name+"/i2p"):
            os.makedirs(self.config.config_folder_name+"/i2p")

        with open(self.config.config_folder_name+"/i2p/i2p-private-key", "w") \
          as private_key_file:
            private_key_file.write(TestFixtures.INJECTOR_I2P_PRIVATE_KEY)

class OuinetIPFSCacheInjector(OuinetInjector):
    """
    As above, but for the 'injector which cache data' 
    """
    def __init__(self, injector_config, deferred_events):        
        super(OuinetIPFSCacheInjector, self).__init__(injector_config, deferred_events)
        self.set_process_protocol(OuinetIPFSCacheProcessProtocol(proc_config = self.config, benchmark_regexes = injector_config.benchmark_regexes, benchmark_deferreds=deferred_events)) # change default protocol to one understand ipfs cache outputs

    def get_IPNS_ID(self):
        return self._proc_protocol.IPNS_ID
    #   self._setup_ipfs_identity()
        
    # def _setup_ipfs_identity(self):
    #     pdb.set_trace()
    #     if not os.path.exists(self.config.config_folder_name+"/ipfs"):
    #         os.makedirs(self.config.config_folder_name+"/ipfs")

    #     with open(self.config.config_folder_name+"/ipfs/config", "w") \
    #       as ipfs_config_file:
    #         print json.dumps(TestFixtures.IPFS_CONFIG_DIC)
    #         ipfs_config_file.write(json.dumps(TestFixtures.IPFS_CACHE_READY_REGEX))
