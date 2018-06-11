# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire ouinet client and injectors for different tests situation

import re
import os
import logging

from twisted.internet import protocol

from test_fixtures import TestFixtures
import pdb

class OuinetProcessProtocol(protocol.ProcessProtocol, object):
    """
    Protocols are the way to communicate with different players
    in a system in Twisted. This protocol is receiving outputs
    from ouinet client/injector process and report failure 
    in case of fatal error
    """
    """
    Protocols are the way to communicate with different players
    in a system in Twisted. This protocol is receiving outputs
    from ouinet client/injector process and report failure 
    in case of fatal error
    """
    def __init__(self, proc_config, ready_benchmark_regex = "", ready_deferred = None):
        super(OuinetProcessProtocol, self).__init__()
        self._ready_benchmark_regex = ready_benchmark_regex
        self._ready_deferred = ready_deferred
        self._proc_config = proc_config
        
    def errReceived(self, data):
        """
        listen for the debugger output reacto to fatal errors and other clues
        """
        logging.debug(self.app_name + ": " + data)
        if re.match(TestFixtures.FATAL_ERROR_INDICATOR_REGEX, data):
            pdb.set_trace()
            raise Exception, "Fatal error"

        if self._ready_deferred and self.check_got_ready(data):
            self._ready_deferred.callback(self)

    def check_got_ready(self, data):
        if self._ready_benchmark_regex:
            return re.match(self._ready_benchmark_regex, data)
            
    def outReceived(self, data):
        logging.debug(self.app_name + ": " + data)

    def processExited(self, reason):
        self.onExit.callback(self)
        #delete the pid file if still exists
        process_pid_file = self._proc_config.config_folder_name + "/pid"
        if os.path.exists(process_pid_file):
            os.remove(process_pid_file)
        
