#!/bin/bash

set -e

export SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")

# Note, this script assumes that you built `ouinet` in the
# `${CMAKE_SOURCE_DIR}/build` directory. If not, modify
# the BUILD variable below.

ROOT=$SCRIPT_DIR/..
BUILD=$ROOT/build
GNUNET_ROOT=$BUILD/gnunet-channels/src/gnunet-channels-build

export PATH=$GNUNET_ROOT/gnunet/bin:$PATH

CLIENT_HOME=$ROOT/repos/client/gnunet

export CLIENT_CFG=$CLIENT_HOME/peer.conf

rm -rf $CLIENT_HOME/.local/share/gnunet/peerinfo/hosts

trap "pkill 'gnunet-*' -9 || true" INT EXIT

GNUNET_TEST_HOME=$CLIENT_HOME   gnunet-arm -s -c $CLIENT_CFG &

sleep 1

echo "* Client's info"
gnunet-peerinfo -s -c $CLIENT_CFG

echo "Press Enter to stop services and exit."

read
