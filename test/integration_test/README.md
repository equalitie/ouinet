# Ouinet integration tests

Ouinet integration tests are implemented in python twisted framework.


## How to run the integration tests

You need to set `OUINET_BUILD_DIR` environmental variable, for example:

```
$ export OUINET_BUILD_DIR=$HOME/ouinet/build/
```

Then you could run the tests by

```
$ cd test/integration_test
$ python -m twisted.trial ./test_http.py
```

or to run  a single test:
```
$ python -m twisted.trial test_http.OuinetTests.test_i2p_i2cp_server
```
to run the speedtests, use:

$ python -m twisted.trial test_http.OuinetTests.test_i2p_transport_speed_1KB

or

$ python -m twisted.trial test_http.OuinetTests.test_i2p_transport_speed_1MB

and look for the line:

`Retrieving 1.049e+06 bytes through I2P tunnel took 15.810519 seconds..`


## I2P Browser tests

I2P Browser test is designed to demonstrate the ability of browsing the internet using Ouinet as an i2p outproxy. To run this test, like other integration tests, you need to set `OUINET_BUILD_DIR` environmental variable:
```
$ export OUINET_BUILD_DIR=$HOME/ouinet/build/
```

and then

```
$ cd test/integration_test
$ python i2p_browser_test.py
```

You need to wait first for the injector i2p tunnel and then for the client tunnel to get established. When both tunnels are established, you need to set your browser to use `127.0.0.1:3888` as both HTTP and HTTPS proxy.

Then you need to tell your browser to trust Ouinet Authority Certificate which could be found at `ouinet/test/integration_test/repos/i2p_client/ssl-ca-cert.pem`.

from there on you should be able to use your browser to browser websites through Ouinet through an anonymous I2P tunnel.

### I2P Browser tests without relaying on the Python script.

The `i2p_browser_test.py` test print the parameters you need to run the injector and client to act as HTTP over I2P proxy. Using these parameters then you could run the injector and the client on different machine to simulate more of a real world situation.

More specifically to run the ouinet injector on the injector machine, create the sub-folder folder `repos/i2p_injector_i2p` in current folder and then run

run the following command:

```
$ $OUINET_BUILD_DIR/injector --repo repos/i2p_injector_i2p --listen-on-i2p true --i2p-hops-per-tunnel 3 --log-level DEBUG
```

The injector will announce its public identity in standard output:
```
[INFO] I2P public ID: 4v4qeW0wYXR-WAEkIXoxVzAMOGNfp42DmlwK9SQf1UK776S1H-SuyGafo6Y3Dkduvx~W2unTWUPW-zZTvlv1V67iVUEwu3mIbpCx3UNCSAZnJ7UY04FrRTYy3VQfR0Lsbq58HzwR7h1GgUXI24XJR9yyz9F65j~j3FsVIgstwOyfB1uc55k-P2x-bI-RIzWwWp1B7esdPdwRuxNMjaUSxUS17bAhzTWmXiJ0nDoU~59UB0IBmQ9DpiM85BnaDuCwnkBdwsy4URJSAASvE0DzE~PVnURxhjfrljgDqP6OGDvVDNpRMqTQLzK3WwaNjb-XoZsPccVj5hP8krYynduDikuCfbfye6NzNXN6NYSfGLprkBobd2aR29cZB6f7zWd4LYDCgAvjAcuDw7g6DWmwf1ZUzfgd3mecO4dTWMABz8DQTeW9LvkKm~gkdb5BPmeP9w2bOF0ISqlkoxsj7LkWKmmdIwWfTbnfnwewoeAziWYq~T8474AJBNgQ4T4tLMloBQAEAAEAAA==
```

You need this to take note of this identity to tell the i2p client about it. Then on client machine run:

```
$ $OUINET_BUILD_DIR/client --repo repos/i2p_client --disable-origin-access --disable-cache --listen-on-tcp 127.0.0.1:3888 --injector-ep i2p:4v4qeW0wYXR-WAEkIXoxVzAMOGNfp42DmlwK9SQf1UK776S1H-SuyGafo6Y3Dkduvx~W2unTWUPW-zZTvlv1V67iVUEwu3mIbpCx3UNCSAZnJ7UY04FrRTYy3VQfR0Lsbq58HzwR7h1GgUXI24XJR9yyz9F65j~j3FsVIgstwOyfB1uc55k-P2x-bI-RIzWwWp1B7esdPdwRuxNMjaUSxUS17bAhzTWmXiJ0nDoU~59UB0IBmQ9DpiM85BnaDuCwnkBdwsy4URJSAASvE0DzE~PVnURxhjfrljgDqP6OGDvVDNpRMqTQLzK3WwaNjb-XoZsPccVj5hP8krYynduDikuCfbfye6NzNXN6NYSfGLprkBobd2aR29cZB6f7zWd4LYDCgAvjAcuDw7g6DWmwf1ZUzfgd3mecO4dTWMABz8DQTeW9LvkKm~gkdb5BPmeP9w2bOF0ISqlkoxsj7LkWKmmdIwWfTbnfnwewoeAziWYq~T8474AJBNgQ4T4tLMloBQAEAAEAAA== --i2p-hops-per-tunnel 3 --log-level
```
Where the parameter after `--injector-ep i2p:` is the i2p public identity  you have gotten from the injector standard output.

From here on setup your browser proxy and trusted certificate as described in previous section and you should be able to browse the internet over i2p using Ouinet.

