# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire ouinet client and injectors for different tests situation

import re

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
    def __init__(self):
        super(OuinetI2PEnabledProcessProtocol, self).__init__()
        self._i2p_ready_deferred = None
        
    def set_i2p_is_ready_object(self, i2p_is_ready_deferred):
        self._i2p_ready_deferred = i2p_is_ready_deferred
    
    def errReceived(self, data):
        """
        listen for the debugger output calls the parent function and
        then check on i2p related messages
        """
        super(OuinetI2PEnabledProcessProtocol, self).errReceived(data)

        if self.client_tunnel_is_ready(data) or self.injector_tunnel_is_ready(data):
           if self._i2p_ready_deferred:
               self._i2p_ready_deferred.callback(self)

    def client_tunnel_is_ready(self, data):
        return re.match(TestFixtures.I2P_TUNNEL_READY_REGEX, data)

    def injector_tunnel_is_ready(self, data):
        return re.match(TestFixtures.I2P_TUNNEL_READY_REGEX, data)
