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
    def __init__(self, app_name, config_file_name, config_file_content, timeout):
        """
        perform the initialization tasks common between all clients 
        and injectors:
        - makes a basic config folder and file 
        - sets the timeout for the process 

        Args
        timeout: the timeout for the lifetime of the process in  seconds, be 
                 used to kill the process if it doesn't finish its tasks by 
                 that timeout 
    
        app_name: a name for the process uses for naming the config folder to
                  preventing config overlapping
     
        config_file_content: the content of config file to be written as text
        """
        self.timeout = timeout
        self.app_name = app_name #used for naming the app config folder
        self.setup_config(config_file_name):

    def setup_config(config_file_name):
        """
        setups various configs, write a fresh config file and make 
        the process protocol object
        """
        #making the necessary folders for the configs and cache
        self.make_config_folder()

        #we need to make a minimal configuration file
        #we overwrite any existing config file to make
        #the test canonical (also trial delete its temp
        #folder each time
        with open(self.config_folder + "/" + TestFixtures.CONFIG_FILE_NAME, "w") as conf_file:
            conf_file.write(config_file_content)

    def make_config_folder(self):
        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME)

        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name)

        self.config_folder = TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name
        
    def start(self, argv, process_protocol = None):
        """
        starts the actual process using twisted spawnProcess

        Args
          argv: command line arguments where argv[0] should be the name of the
                executable program with its code
          process_protoco: twisted process protocol which deals with processing 
                           the output of the ouinet process. If None, a generic
                           Ouinet Process
        """
        #do not start twice
        if hasattr(self, '_has_started') and self._has_started:
            return

        if not process_potocol:
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
        listen for the debugger output reacto to fatal errors and other clues
        """
        if re.match(r'\[ABORT\]', data):
            raise Exception, "Fatal error"
            
    def processExited(self, reason):
        self.onExit.callback(self)

class OuinetClient(OuinetProcess):
    def __init__(self, client_name, i2p_ready = None):
        super(OuinetClient(client_name, "ouinet-client.conf", TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT, timeout)

        argv = [ouinet_env['OUINET_BUILD_DIR'] + "client", "--repo", self.config_folder]
        argv.extend(args)

        self.start(argv)

# As above, but for the 'injector' 
class OuinetInjector(OuinetProcess):
    """
    Starts an injector process by passing the args to the service

    Args
    injector_name: the name of the injector which determines the config folder name
    timeout: how long before killing the injector process
    args: list containing command line arguments passed directly to the injector
    ready_deferred: is a Deferred object whose callback is being called when the 
                    injector is ready to serve client
    
    """
    def __init__(self, injector_name, timeout, args, ready_deferred = None):
        self.setup_config(injector_name, timeout, "ouinet-injector.conf", TestFixtures.INJECTOR_CONF_FILE_CONTENT, ready_deferred)

        argv = [ouinet_env['OUINET_BUILD_DIR'] + "injector", "--repo", self.config_folder]
        argv.extend(args)

        self.start(argv)


class OuinetI2PInjector(OuineInjector):
    """
    It is a child of OuinetInjector with i2p ouiservice 

    Args
    injector_name: the name of the injector which determines the config folder name
    timeout: how long before killing the injector process
    args: list containing command line arguments passed directly to the injector
    i2p_ready: is a Deferred object whose callback is being called when i2p tunnel is ready

    
    """
    def __init__(self, injector_name, timeout, args, i2p_ready):
        self.setup_i2p_private_key()
        super(OuinetInjector, self).__init__(injector_name, timeout, args)

    def _setup_ip2_private_key(self):
        if not os.path.exists(self.config_folder+"/i2p"):
            os.makedirs(self.config_folder)

        with open(self.config_folder+"/i2p/i2p-private-key", "w") as private_key_file:
            private_key_file.write(TestFixtures.INJECTOR_I2P_PRIVATE_KEY)

class OuinetI2PInjector(OuinetI2PInjector):
    """
    It is a child of OuinetInjector which check for the request to be cached 
    and trigger a deferred object

    Args
    injector_name: the name of the injector which determines the config folder name
    timeout: how long before killing the injector process
    args: list containing command line arguments passed directly to the injector
    benchmark_regexes: is an arry of two Deferred object the first's callback is being 
    called when the the tcp port of the injector is ready. The second will be called
    when firs request gets cached

    
    """
    def __init__(self, injector_name, timeout, args, i2p_ready):
        super(OuinetInjector, self).__init__(injector_name, timeout, args)

    def _setup_ip2_private_key(self):
        if not os.path.exists(self.config_folder+"/i2p"):
            os.makedirs(self.config_folder)

        with open(self.config_folder+"/i2p/i2p-private-key", "w") as private_key_file:
            private_key_file.write(TestFixtures.INJECTOR_I2P_PRIVATE_KEY)

        


