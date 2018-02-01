#!/bin/bash

set -e

export SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")

. $SCRIPT_DIR/gnunet-env.sh

if [ -z "$1" ]; then
    echo "The first argument must be a GNUnet ID of the acceptor"
    exit 1
fi

echo "Our GNUnet ID is `client_id`"
echo "Connecting to $1"
gnunet-cadet -c $CLIENT_CFG $1 cadet-test-port

