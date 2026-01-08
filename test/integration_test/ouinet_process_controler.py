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
from subprocess import TimeoutExpired
from subprocess import Popen, PIPE, STDOUT, check_output

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


def spawn(command: List[str]) -> Popen:
    print("running command :", " ".join(command))
    handle = Popen(
        command,
        shell=False,
        stdin=PIPE,
        stdout=PIPE,
        stderr=STDOUT,
    )
    if handle.stdout is None:
        raise ValueError("could not run command: ", command)
    return handle


def output_yielder(handle: Popen) -> Generator[str, None, None]:
    with handle:
        try:
            if not handle.stdout:
                raise IOError("no stdout on process")
            for line in iter(handle.stdout.readline, ""):
                if isinstance(line, bytes):
                    line = line.decode("utf-8")
                yield line
        except:
            pass


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
        """
        self.config = ouinet_config
        # in case the communication process protocol is not explicitly set
        # starts a default process protocol to check on Fatal errors
        # TODO: This option apparently has been removed. Check if we ever need
        # preset process protocol.
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

    def abort_task_because_exception(self, task: asyncio.Task):
        """
        Using this makes sure we have enough debugging information
        """
        if not task.done():
            raise ValueError(f"Task {task} failed to mark itself done when aborting")
        if not task.exception():
            raise ValueError(f"Task {task} failed to set up exception when aborting")

        print(f"Properly aborted the task {task}, now stopping the loop")
        loop = asyncio.get_running_loop()
        loop.stop()

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
                executable binary
          process_protocol: deals with processing the output of the ouinet process.
                If None, a generic Ouinet Process
        """
        # do not start twice
        if self._has_started:
            return

        self._proc_protocol.app_name = self.config.app_name

        self.command = self.config.argv
        print("Spawning process " + " ".join(self.command))

        self._proc = spawn(self.command)
        self.output = output_yielder(self._proc)
        self.listener: asyncio.Task = asyncio.create_task(self.stdout_listening_task())
        print("spawned")

        self._has_started = True

    def assert_process_is_alive(self):
        if self._proc.poll() is not None:
            print("sudden death of ", self.command[0])
            stdout, stderr = self._proc.communicate(timeout=5)
            raise IOError(
                "process",
                self.command,
                "has unexpectedly died, stdout:",
                stdout,
                "stderr:",
                stderr,
            )

    async def stdout_listening_task(self):
        try:
            while True:
                if not self._term_signal_sent:
                    self.assert_process_is_alive()
                else:
                    return

                line: str = await asyncio.to_thread(self.output.__next__)
                assert isinstance(line, str)
                self._proc_protocol.errReceived(line)

            print("exited listening loop for ", self)
        except asyncio.CancelledError:
            print("Cancelled listening to process ", self.command[0])
            return

        except Exception as e:
            # stopping the test to return the error
            print("setting an exception for ", self.command)

            task = asyncio.current_task()
            asyncio.get_running_loop().call_soon(
                self.abort_task_because_exception, task
            )
            print("scheduled loopstop")
            raise IOError("Error while listening to stdout") from e

    def send_term_signal(self):
        if not self._term_signal_sent:
            logging.debug("Sending term signal to " + self.config.app_name)
            self._term_signal_sent = True
            self._proc.terminate()

    async def stop_listening(self):
        listener = self.listener
        if listener.done() and listener.exception():
            raise IOError("Error while running the process:") from listener.exception()
        print("no error reported, cancelling listening task")
        if not listener.get_loop().is_closed():
            listener.cancel()
            await listener

    def is_teardown(self) -> bool:
        tb = "\n".join(format_stack())
        if "teardown" in tb.lower():
            return True
        return False

    async def stop(self):
        if self._has_started and not self._term_signal_sent:  # stop only if started
            self._has_started = False
            logging.debug("process " + self.config.app_name + " stopping")
            print("process " + self.config.app_name + " stopping")

            # Introspection for extra debug details
            if self.is_teardown():
                print("this is happening as a part of teardown")
            else:
                print("the process is being stopped from inside the test body")

            await self.stop_listening()

            self.send_term_signal()
            print("waiting for termination of", self.command[0])
            # Waiting for the process to actually terminate
            try:
                self._proc.wait(timeout=5)
            except TimeoutExpired:
                print("[WARNING] termination unsuccessfull, killing", self.command[0])
                self._proc.kill()
                self._proc.communicate()

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
    def __init__(self, client_config: OuinetConfig):
        super(OuinetCacheClient, self).__init__(client_config)
        self.set_process_protocol(
            OuinetBEP5CacheProcessProtocol(
                proc_config=self.config,
                benchmark_regexes=client_config.benchmark_regexes,
            )
        )

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
    """

    def __init__(self, injector_config):
        injector_config.config_file_name = TestFixtures.INJECTOR_CONF_FILE_NAME
        injector_config.config_file_content = TestFixtures.INJECTOR_CONF_FILE_CONTENT
        super(OuinetInjector, self).__init__(injector_config)
        # Isn't it supposed to do it BEFORE initializing the superclass?
        self.config.argv = [
            OuinetInjector.injector_path(),
            "--repo",
            self.config.config_folder_name,
        ] + self.config.argv

    @staticmethod
    def has_i2p() -> bool:
        output = check_output([OuinetInjector.injector_path(), "--help"]).decode(
            "utf-8"
        )
        return "i2p" in output

    @staticmethod
    def injector_path() -> str:
        """Helper function for injector capabilities check"""
        return os.path.join(ouinet_env["OUINET_BUILD_DIR"], "injector")

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
        return self._proc_protocol.bep5_public_key


class OuinetI2PInjector(OuinetInjector):
    """
    As above, but for the 'injector'
    It is a child of Ouinetinjector with i2p ouiservice
    """

    def __init__(self, injector_config, private_key_blob=None):
        super(OuinetI2PInjector, self).__init__(injector_config)
        # we use cache process protocol to be able to store
        # cache public key in case we need it
        self.set_process_protocol(
            OuinetBEP5CacheProcessProtocol(
                proc_config=self.config,
                benchmark_regexes=injector_config.benchmark_regexes,
            )
        )

        self._setup_i2p_private_key(private_key_blob)

    def _setup_i2p_private_key(self, private_key_blob):
        if not os.path.exists(self.config.config_folder_name + "/i2p"):
            os.makedirs(self.config.config_folder_name + "/i2p")

        if private_key_blob:
            with open(
                self.config.config_folder_name + "/i2p/i2p-private-key", "w"
            ) as private_key_file:
                private_key_file.write(private_key_blob)

    def get_I2P_public_ID(self):
        try:
            with open(
                self.config.config_folder_name + "/endpoint-i2p", "r"
            ) as public_id_file:
                return public_id_file.read().rstrip()
        except:
            return None

    def get_index_key(self):
        return self._proc_protocol.bep5_public_key
