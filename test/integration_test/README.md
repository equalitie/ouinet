# Ouinet integration tests

Ouinet integration tests are implemented in python twisted framework.


## How to run the integration tests

You need to set `OUINET_BUILD_DIR` enviornmental variable, for example:

```
$ export OUINET_BUILD_DIR=$HOME/ouinet/build/
```

Then you could run the tests by

```
$ cd test/integration_test
$ python -m twisted.trial ./test_http.py
```

or to run  a single test
```
$ python -m twisted.trial test_http.OuinetTests.test_i2p_i2cp_server
```
