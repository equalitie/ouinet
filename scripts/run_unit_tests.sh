#/usr/bin/env bash

if [ -z "$OUINET_REPO_DIR" ]; then
    if [ ! -d ".git" ]; then
        echo "Please either set OUINET_REPO_DIR to point to the root of Ouinet repository or run the script from the root of Ouinet repository:"
        echo "./scripts/run_integration_tests.sh"
        exit 1
    else
        OUINET_REPO_DIR=`pwd`
    fi
fi

[ -z "$OUINET_BUILD_DIR" ] && OUINET_BUILD_DIR="$OUINET_REPO_DIR/scripts/ouinet-local-build/"
if [ ! -f "$OUINET_BUILD_DIR/client" ] || [ ! -f "$OUINET_BUILD_DIR/injector" ]; then
    echo "Cannot find the client or the injector in the build directory $OUINET_BUILD_DIR"
    echo "Please update the OUINET_BUILD_DIR env variable to point to Ouinet build directory"
    exit 1
fi

TESTPATH=$OUINET_BUILD_DIR/test

for test in $TESTPATH/test-*; do
    "$test" --log_level=test_suite
done
