## Running integration tests

The Ouinet source comes with a set of integration tests.  To run them you will
need the [Twisted](https://twistedmatrix.com/) Python framework.

If you already built Ouinet from `<SOURCE DIR>` into `<BUILD DIR>` (see
building from source instructions), you can run the tests as follows:

    $ export OUINET_REPO_DIR=<SOURCE DIR>
    $ export OUINET_BUILD_DIR=<BUILD DIR>
    $ ./scripts/run_integration_tests.sh

