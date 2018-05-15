# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire ouinet client and injectors for different tests situation

import re

from twisted.internet import protocol

import pdb

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
        print self.app_name, data
        if re.match(r'\[ABORT\]', data):
            raise Exception, "Fatal error"
            
    def outReceived(self, data):
        print self.app_name, data

    def processExited(self, reason):
        self.onExit.callback(self)

class OuinetI2PEnabledProcessProtocol(OuinetProcessProtocol):
    """
    Protocols are the way to communicate with different players
    in a system in Twisted. This protocol is receiving outputs
    from ouinet client/injector process and report failure 
    in case of fatal error
    """
    def set_i2p_is_ready_object(self, i2p_is_ready_deferred):
        self._i2p_ready_deferred = i2p_is_ready_deferred
    
    def errReceived(self, data):
        """
        listen for the debugger output calls the parent function and
        then check on i2p related messages
        """
        super(OuinetI2PEnabledProcessProtocol, self).errReceived(self, data)

        if re.match(r'\[DEBUG\] I2P Tunnel to the injector is established', data) and hasattr(self, _i2p_ready_deferred):
            self._i2p_ready_deferred.callback(self)

