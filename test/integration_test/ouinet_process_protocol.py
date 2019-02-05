# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire ouinet client and injectors for different tests situation

import re
import os
import time

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
    def __init__(self, proc_config, ready_benchmark_regex="", ready_deferred=None):
        super(OuinetProcessProtocol, self).__init__()
        self._ready_benchmark_regex = ready_benchmark_regex
        self._ready_deferred = ready_deferred
        self._proc_config = proc_config
        self._got_ready = False

        self._logger = logging.getLogger()

    def errReceived(self, data):
        """
        listen for the debugger output reacto to fatal errors and other clues
        """
        logging.debug(self.app_name + ": " + data)
        self._logger.handlers[0].flush()

        if re.match(TestFixtures.FATAL_ERROR_INDICATOR_REGEX, data):
            raise Exception("Fatal error")

        if (not self._got_ready) and self._ready_deferred and self.check_got_ready(data):
            self._got_ready = True
            self._ready_deferred.callback(self)

    def check_got_ready(self, data):
        if self._ready_benchmark_regex:
            return re.match(self._ready_benchmark_regex, data)

        return False

    def outReceived(self, data):
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
    def __init__(self, proc_config, benchmark_regexes=[], benchmark_deferreds=None):
        super(OuinetCacheProcessProtocol, self).__init__(proc_config,
                benchmark_regexes[TestFixtures.READY_REGEX_INDEX],
                benchmark_deferreds[TestFixtures.READY_REGEX_INDEX])

        self._index_ready_deferred = None
        self._request_cached_deferred = None
       
        # TODO: this need to change to dictionary
        if (len(benchmark_regexes) > TestFixtures.INDEX_READY_REGEX_INDEX):
            self._index_ready_regex = benchmark_regexes[TestFixtures.INDEX_READY_REGEX_INDEX]
            self._index_ready_deferred = benchmark_deferreds[TestFixtures.INDEX_READY_REGEX_INDEX]
        if (len(benchmark_regexes) > TestFixtures.REQUEST_CACHED_REGEX_INDEX):
            self._request_cached_regex = benchmark_regexes[TestFixtures.REQUEST_CACHED_REGEX_INDEX]
            self._request_cached_deferred = benchmark_deferreds[TestFixtures.REQUEST_CACHED_REGEX_INDEX]
        self._number_of_cache_db_updates = 0
        self._served_from_cache = False

    def errReceived(self, data):
        """
        listen for the debugger output calls the parent function and then react to cached request cached
        """
        #checking for specifc strings before calling back any deferred object
        #because the reaction to the deferred might depend on these data
        self.check_response_served_from_cached(data)

        super(OuinetCacheProcessProtocol, self).errReceived(data)

        if self._index_ready_deferred and self.check_index_ready(data):
            self._index_ready_deferred.callback(self)

        if self._request_cached_deferred and self.check_request_got_cached(data):
            self._number_of_cache_db_updates += 1
            if self._number_of_cache_db_updates == \
               TestFixtures.NO_OF_CACHED_MESSAGES_REQUIRED:
                self._request_cached_deferred.callback(self)

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


class OuinetIPFSCacheProcessProtocol(OuinetCacheProcessProtocol, object):
    def __init__(self, proc_config, benchmark_regexes=[], benchmark_deferreds=None):
        super(OuinetIPFSCacheProcessProtocol, self).__init__(
            proc_config, benchmark_regexes, benchmark_deferreds)
        self.IPNS_ID = ""
        self.IPNS_resolution_start_time = 0

    def errReceived(self, data):
        self.Mark_start_of_first_IPNS_resolution(data)
        self.look_for_IPNS_ID(data)
        super(OuinetIPFSCacheProcessProtocol, self).errReceived(data)

    def look_for_IPNS_ID(self, data):
        IPNS_ID_search_result = re.match(TestFixtures.IPNS_ID_ANNOUNCE_REGEX, data)
        if IPNS_ID_search_result:
            self.IPNS_ID = IPNS_ID_search_result.group(1)

    def Mark_start_of_first_IPNS_resolution(self, data):
        if self.IPNS_resolution_start_time == 0 and re.match(TestFixtures.START_OF_IPNS_RESOLUTION_REGEX, data):
            self.IPNS_resolution_start_time = time.time()


class OuinetBEP44CacheProcessProtocol(OuinetCacheProcessProtocol, object):
    def __init__(self, proc_config, benchmark_regexes=[], benchmark_deferreds=None):
        super(OuinetBEP44CacheProcessProtocol, self).__init__(
            proc_config, benchmark_regexes, benchmark_deferreds)
        self.BEP44_pubk = ""

    def errReceived(self, data):
        self.look_for_BEP44_pubk(data)
        super(OuinetBEP44CacheProcessProtocol, self).errReceived(data)

    def look_for_BEP44_pubk(self, data):
        BEP44_pubk_search_result = re.match(TestFixtures.BEP44_PUBK_ANNOUNCE_REGEX, data)
        if BEP44_pubk_search_result:
            self.BEP44_pubk = BEP44_pubk_search_result.group(1)
