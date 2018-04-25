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

TIMEOUT_LEN = 15 # seconds

# Helper: stick "| " at the beginning of each line of |s|.

def indent(s):
    return "| " + "\n| ".join(s.strip().split("\n"))

# Helper: generate unified-format diffs between two named strings.
# Pythonic escaped-string syntax is used for unprintable characters.

def diff(label, expected, received):
    if expected == received:
        return ""
    else:
        return (label + "\n| "
                + "\n| ".join(s.encode("string_escape")
                              for s in
                              difflib.unified_diff(expected.split("\n"),
                                                   received.split("\n"),
                                                   "expected", "received",
                                                   lineterm=""))
                + "\n")

# Helper: Run ouinet client instances and confirm that they have
# completed without any errors.

# set MALLOC_CHECK_ in subprocess environment; this gets us
# better memory-error behavior from glibc and is harmless
# elsewhere.  Mode 2 is "abort immediately, without flooding
# /dev/tty with useless diagnostics" (the documentation SAYS
# they go to stderr, but they don't).

ouinet_env = {}
ouinet_env.update(os.environ)
ouinet_env['MALLOC_CHECK_'] = '2'

class OuinetProcess(subprocess.Popen):
    def make_config_folder(self):
        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME)

        if not os.path.exists(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name):
            os.makedirs(TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name)

        self.config_folder = TestFixtures.REPO_FOLDER_NAME + "/" + self.app_name

    
    def check_for_fatal_error(self):
        """
        Reads the first unread line from stderr and check if the ouinet client 
        has thrown an error

        Returns:
            True if a fatal error has occurred False otherwise 
        """
        line = self.stderr.readline()
        #pdb.set_trace()
        if re.match(r'stderr', line):
            print line
            return True
  
        return False

    def setup_config(self, app_name, config_file_name, config_file_content):
        #making the necessary folders for the configs and cache
        self.app_name = app_name
        self.make_config_folder()

        #we need to make a minimal configuration file
        #we overwrite any existing config file to make
        #the test canonical (also trial delete its temp
        #folder each time
        with open(self.config_folder + "/" + config_file_name, "w") as conf_file:
            conf_file.write(config_file_content)

    def stop(self):
        if self.poll() is None:
            self.terminate()
            print self.app_name," stopped"

class OuinetClient(OuinetProcess):
    def __init__(self, client_name, *args, **kwargs):
        #making the necessary folders for the configs and cache
        self.setup_config(client_name, "ouinet-client.conf", TestFixtures.FIRST_CLIENT_CONF_FILE_CONTENT)

        argv = [ouinet_env['OUINET_BUILD_DIR'] + "client", "--repo", self.config_folder]
        
        if len(args) == 1 and (isinstance(args[0], list) or
                               isinstance(args[0], tuple)):
            argv.extend(args[0])
        else:
            argv.extend(args)

        subprocess.Popen.__init__(self, argv,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  env=ouinet_env,
                                  close_fds=True,
                                  **kwargs)

        # wait for startup completion, which is signaled by
        # the subprocess closing its stdout
        self.check_for_fatal_error()
        #self.output = self.stdout.read()
        
        # read stderr in a separate thread, since we will
        # have several processes outstanding at the same time
        self.communicator = threading.Thread(target=self.run_communicate)
        self.communicator.start()
        self.timeout = threading.Timer(TIMEOUT_LEN, self.stop)
        self.timeout.start()

    severe_error_re = re.compile(
        r"\[(?:warn|err(?:or)?)\]|ERROR SUMMARY: [1-9]|LEAK SUMMARY:")

    def run_communicate(self):
        self.errput = self.stderr.read()
        
    def check_completion(self, label, force_stderr=False):
        self.stdin.close()
        self.communicator.join()
        if self.poll() is not None:
            self.timeout.cancel()
        self.timeout.join()
        self.wait()

        report = ""

        # exit status should be zero
        if self.returncode > 0:
            report += label + " exit code: %d\n" % self.returncode
        elif self.returncode < 0:
            report += label + " killed: signal %d\n" % -self.returncode

        # there should be nothing on stdout
        if self.output != "":
            report += label + " stdout:\n%s\n" % indent(self.output)

        # there will be debugging messages on stderr, but there should be
        # no [warn], [err], or [error] messages.
        if (force_stderr or
            self.severe_error_re.search(self.errput) or
            self.returncode != 0):
            report += label + " stderr:\n%s\n" % indent(self.errput)

        return report

# As above, but for the 'injector' 
class OuinetInjector(OuinetProcess):
    def __init__(self, injector_name, extra_args=(), **kwargs):
        self.setup_config(injector_name, "ouinet-injector.conf", TestFixtures.INJECTOR_CONF_FILE_CONTENT)
        argv = [ouinet_env['OUINET_BUILD_DIR'] + "injector", "--repo", self.config_folder]
        argv.extend(extra_args)

        subprocess.Popen.__init__(self, argv,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  env=ouinet_env,
                                  close_fds=True,
                                  **kwargs)
        # wait for startup completion, which is signaled by
        # the subprocess closing its stdout
        self.check_for_fatal_error()

        # invoke communicate() in a separate thread, since we will
        # have several processes outstanding at the same time
        self.communicator = threading.Thread(target=self.run_communicate)
        self.communicator.start()
        self.timeout = threading.Timer(TIMEOUT_LEN, self.stop)
        self.timeout.start()

    def run_communicate(self):
        (out, err) = self.communicate()
        self.output = out
        self.errput = err

    def check_completion(self, label):
        self.communicator.join()
        self.timeout.cancel()
        self.timeout.join()
        self.poll()

        # exit status should be zero, and there should be nothing on
        # stderr
        if self.returncode != 0 or self.errput != "":
            report = ""
            # exit status should be zero
            if self.returncode > 0:
                report += label + " exit code: %d\n" % self.returncode
            elif self.returncode < 0:
                report += label + " killed: signal %d\n" % -self.returncode
            if self.errput != "":
                report += label + " stderr:\n%s\n" % indent(self.errput)
            raise AssertionError(report)

        # caller will crunch the output
        return self.output

# As above, but for the 'tester-proxy' which simulate an http proxy between
# stegotorus server and client. 
class TesterProxy(subprocess.Popen):
    def __init__(self, extra_args=(), **kwargs):
        """
        Initiates the subprocess and based of args it decides how to manipulate the 
        headers of the http packets
        """
        argv = ["./tester_proxy"]
        argv.extend(extra_args)

        subprocess.Popen.__init__(self, argv,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  close_fds=True,
                                  **kwargs)
        # wait for startup completion, which is signaled by
        # the subprocess closing its stdout
        self.output = self.stdout.read()

        # invoke communicate() in a separate thread, since we will
        # have several processes outstanding at the same time
        self.communicator = threading.Thread(target=self.run_communicate)
        self.communicator.start()
        self.timeout = threading.Timer(TIMEOUT_LEN, self.stop)
        self.timeout.start()

    def stop(self):
        if self.poll() is None:
            self.terminate()

    def run_communicate(self):
        (out, err) = self.communicate()
        self.output = out
        self.errput = err

    def check_completion(self, label):
        self.communicator.join()
        self.timeout.cancel()
        self.timeout.join()
        self.poll()

        # exit status should be zero, and there should be nothing on
        # stderr
        if self.returncode != 0 or self.errput != "":
            report = ""
            # exit status should be zero
            if self.returncode > 0:
                report += label + " exit code: %d\n" % self.returncode
            elif self.returncode < 0:
                report += label + " killed: signal %d\n" % -self.returncode
            if self.errput != "":
                report += label + " stderr:\n%s\n" % indent(self.errput)
            raise AssertionError(report)

        # caller will crunch the output
        return self.output


    
