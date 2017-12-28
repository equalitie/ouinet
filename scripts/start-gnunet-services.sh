#!/bin/bash

set -e

export SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")

# Note, this script assumes that you built `ouinet` in the
# `${CMAKE_SOURCE_DIR}/build` directory and that client and injector repos sit
# under `${CMAKE_SOURCE_DIR}/repos`. If not, modify the BUILD and REPOS
# variables below.

ROOT=$SCRIPT_DIR/..
BUILD=${BUILD:-$ROOT/build}
REPOS=${REPOS:-$ROOT/repos}

GNUNET_ROOT=$BUILD/gnunet-channels/src/gnunet-channels-build

export PATH=$GNUNET_ROOT/gnunet/bin:$PATH

CLIENT_HOME=$REPOS/client/gnunet
INJECTOR_HOME=$REPOS/injector/gnunet

export CLIENT_CFG=$CLIENT_HOME/peer.conf
export INJECTOR_CFG=$INJECTOR_HOME/peer.conf

rm -rf $CLIENT_HOME/.local/share/gnunet/peerinfo/hosts
rm -rf $INJECTOR_HOME/.local/share/gnunet/peerinfo/hosts

trap "pkill 'gnunet-*' -9 || true" INT EXIT

GNUNET_TEST_HOME=$CLIENT_HOME   gnunet-arm -s -c $CLIENT_CFG &
GNUNET_TEST_HOME=$INJECTOR_HOME gnunet-arm -s -c $INJECTOR_CFG &

sleep 1

echo "* Client's info"
gnunet-peerinfo -s -c $CLIENT_CFG
echo "* Injector's info"
gnunet-peerinfo -s -c $INJECTOR_CFG

# This is not necessary, but it makes developing easier/faster.
echo "Interconnecting the two..."
gnunet-peerinfo -c $CLIENT_CFG -p `gnunet-peerinfo -c $INJECTOR_CFG -g`

echo "Done"
echo "Press Enter to stop services and exit."

read
