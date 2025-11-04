# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire ouinet client and injectors for different tests situation

import re
from re import Match
import os
import logging

from twisted.internet import protocol
from twisted.internet.defer import Deferred

from typing import List
from test_fixtures import TestFixtures


class OuinetProcessProtocol(protocol.ProcessProtocol, object):
    """
    Protocols are the way to communicate with different players
    in a system in Twisted. This protocol is receiving outputs
    from ouinet client/injector process and report failure
    in case of fatal error
    """

    def __init__(self, proc_config, watchpoint_regexes: List[str]):
        super(OuinetProcessProtocol, self).__init__()
        self.regexes: List[str] = watchpoint_regexes
        self.callbacks: dict[str, Deferred] = {}
        self._proc_config = proc_config

        for regex in self.regexes:
            self.callbacks[regex] = Deferred()

        self._logger: logging.Logger = logging.getLogger()

    def errReceived(self, data):
        """
        listen for the debugger output reacto to fatal errors and other clues
        """
        data = data.decode()
        print("err")
        logging.debug(self.app_name + ": " + data)
        self._logger.handlers[0].flush()

        if re.match(TestFixtures.FATAL_ERROR_INDICATOR_REGEX, data):
            raise Exception("Fatal error")

        for regex in self.callbacks.keys():
            match = re.match(regex, data)
            if match:
                cb: Deferred = self.callbacks[regex]
                if not cb.called:
                    cb.callback(self)

    def outReceived(self, data):
        print("out")
        data = data.decode()
        logging.debug(self.app_name + ": " + data)
        self._logger.handlers[0].flush()

    def pulse_out(self):
        logging.debug(self.app_name + " is alive")

    def processExited(self, reason):
        self.onExit.callback(self)
        # delete the pid file if still exists
        process_pid_file = self._proc_config.config_folder_name + "/pid"
        if os.path.exists(process_pid_file):
            os.remove(process_pid_file)


class OuinetCacheProcessProtocol(OuinetProcessProtocol, object):
    def __init__(self, proc_config, regexes=[]):
        super(OuinetCacheProcessProtocol, self).__init__(
            proc_config,
            watchpoint_regexes=regexes,
        )

        self._number_of_cache_db_updates = 0
        self._served_from_cache = False

    def errReceived(self, data):
        """
        listen for the debugger output calls the parent function and then react to cached request cached
        """
        data, rdata = data.decode(), data
        # checking for specifc strings before calling back any deferred object
        # because the reaction to the deferred might depend on these data
        self.check_response_served_from_cached(data)

        super(OuinetCacheProcessProtocol, self).errReceived(rdata)

    def check_index_ready(self, data):
        if self._index_ready_regex:
            return re.match(self._index_ready_regex, data)

    def check_request_got_cached(self, data):
        if self._request_cached_regex:
            return re.match(self._request_cached_regex, data)

    def check_response_served_from_cached(self, data):
        if re.match(TestFixtures.RETRIEVED_FROM_CACHE_REGEX, data):
            self._served_from_cache = True

    def served_from_cache(self):
        return self._served_from_cache


class OuinetBEP5CacheProcessProtocol(OuinetCacheProcessProtocol, object):
    def __init__(self, proc_config, benchmark_regexes=[]):
        print("initting bep5 proto")
        super(OuinetBEP5CacheProcessProtocol, self).__init__(
            proc_config, benchmark_regexes
        )
        self.public_key = ""

    def errReceived(self, data):
        # print("receiving line", data)

        data, rdata = data.decode(), data
        self.look_for_public_key(data)
        super(OuinetBEP5CacheProcessProtocol, self).errReceived(rdata)

    def look_for_public_key(self, data):
        pubkey_search_result = re.match(TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX, data)
        if pubkey_search_result:
            self.public_key = pubkey_search_result.group(1)
