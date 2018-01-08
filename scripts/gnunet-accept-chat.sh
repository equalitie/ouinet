#!/bin/bash

set -e

export SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")

. $SCRIPT_DIR/gnunet-env.sh

echo "Accepting on `injector_id`"
gnunet-cadet -c $INJECTOR_CFG --open-port cadet-test-port

