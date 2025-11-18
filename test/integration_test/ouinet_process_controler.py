# Copyright 2018, eQualit.ie
# See LICENSE for other credits and copying information

# Integration tests for ouinet - classes which setup and fire
# ouinet client and injectors for different tests situation

import os
import logging
from typing import List
from typing import Generator
from traceback import format_stack
import asyncio

from test_fixtures import TestFixtures

from ouinet_process_protocol import (
    OuinetBEP5CacheProcessProtocol,
    OuinetProcessProtocol,
)

ouinet_env = {}
ouinet_env.update(os.environ)


class OuinetConfig(object):
    """
    a class contains all configuration corresponding
    to a Ouinet (client/injector) process. This is
    to make it easier to send the config to different
    process
    """

    def __init__(
        self,
        app_name="generic ouinet app",
        timeout=TestFixtures.DEFAULT_PROCESS_TIMEOUT,
        argv=[],
        config_file_name="ouinet.conf",
        benchmark_regexes=[],
        config_file_content="",
    ):
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


from subprocess import Popen, PIPE, STDOUT


def spawn(command: List[str]) -> Popen:
    print("running command :", " ".join(command))
    handle = Popen(
        command,
        shell=False,
        stdin=PIPE,
        stdout=PIPE,
        stderr=STDOUT,
        # universal_newlines=True,
    )
    if handle.stdout is None:
        raise ValueError("could not run command: ", command)
    return handle


def output_yielder(handle: Popen) -> Generator[str, None, None]:
    """Note: If you break during iteration, it kills the process"""
    with handle:
        try:
            for line in iter(handle.stdout.readline, ""):
                yield line
        except:
            # yield ""
            pass
        # except GeneratorExit:
        #     handle.kill()


class OuinetProcess(object):
    def __init__(self, ouinet_config):
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

        self._proc_protocol = OuinetProcessProtocol(
            self.config, ouinet_config.benchmark_regexes
        )

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
        with open(
            self.config.config_folder_name + "/" + self.config.config_file_name, "w"
        ) as conf_file:
            conf_file.write(self.config.config_file_content)

    def make_config_folder(self):
        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME)

        if not os.path.exists(
            TestFixtures.REPO_FOLDER_NAME + "/" + self.config.app_name
        ):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME + "/" + self.config.app_name)

        self.config.config_folder_name = (
            TestFixtures.REPO_FOLDER_NAME + "/" + self.config.app_name
        )

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

        print("spawning a process")
        comm = self.config.argv

        self._proc = spawn(comm)
        self.output = output_yielder(self._proc)
        self.listener: asyncio.Task = asyncio.create_task(self.listening_loop())
        # self._proc_protocol, , self.config.argv, env=ouinet_env
        print("spawned")
        self._has_started = True

        # we add a twisted timer to kill the process after timeout
        logging.debug(
            self.config.app_name
            + " times out in "
            + str(self.config.timeout)
            + " seconds"
        )

    async def listening_loop(self):
        try:
            # while True:
            for line in self.output:
                # line = self.output.next()
                # line = await asyncio.wait_for(self.output.next())
                # line = await line
                self._proc_protocol.errReceived(line)
                await asyncio.sleep(0.1)
            print("exited listening loop for ", self)
        except asyncio.CancelledError:
            print("stopping output analysis")

    def send_term_signal(self):
        if not self._term_signal_sent:
            logging.debug("Sending term signal to " + self.config.app_name)
            self._term_signal_sent = True
            self._proc.terminate()

    def stop(self):
        if self._has_started and not self._term_signal_sent:  # stop only if started
            self._has_started = False
            logging.debug("process " + self.config.app_name + " stopping")

            # Introspection for extra debug details
            tb = "\n".join(format_stack())
            if "teardown" in tb.lower():
                logging.debug("this is happening as a part of teardown")
            else:
                logging.debug("the process is being stopped from inside the test body")

            # if self.timeout_killer.active():
            #     self.timeout_killer.cancel()

            # self.pulse_timer.stop()
            self.listener.cancel()
            self.send_term_signal()

            # Don't do this because we may lose some important debug
            # information that gets printed between now and when the app
            # actually exits.
            # self._proc_protocol.transport.loseConnection()

    @property
    def callbacks(self):
        return self._proc_protocol.benchmarks


class OuinetClient(OuinetProcess):
    def __init__(self, client_config: OuinetConfig):
        client_config.config_file_name = "ouinet-client.conf"
        client_config.config_file_content = TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT
        super(OuinetClient, self).__init__(client_config)

        self.config.argv = [
            os.path.join(ouinet_env["OUINET_BUILD_DIR"], "client"),
            "--repo",
            self.config.config_folder_name,
            "--log-level=SILLY",
            "--enable-log-file",
        ] + self.config.argv


class OuinetCacheClient(OuinetClient):
    def served_from_cache(self):
        """
        returns true if any request has been served from cache

        """
        if self._proc_protocol:
            return self._proc_protocol.served_from_cache()

        return False


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
        injector_config.config_file_content = TestFixtures.INJECTOR_CONF_FILE_CONTENT
        super(OuinetInjector, self).__init__(injector_config)
        self.config.argv = [
            os.path.join(ouinet_env["OUINET_BUILD_DIR"], "injector"),
            "--repo",
            self.config.config_folder_name,
        ] + self.config.argv

    def get_index_key(self) -> str:
        """Return a key string used to access the cache index created by this injector."""
        raise NotImplementedError


class OuinetBEP5CacheInjector(OuinetInjector):
    """
    As above, but for the 'injector which cache data' with a BEP5 index
    """

    def __init__(self, injector_config):
        super(OuinetBEP5CacheInjector, self).__init__(injector_config)
        self.set_process_protocol(
            OuinetBEP5CacheProcessProtocol(
                proc_config=self.config,
                benchmark_regexes=injector_config.benchmark_regexes,
            )
        )

    def get_index_key(self):
        return self._proc_protocol.public_key
