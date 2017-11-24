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

CH=$ROOT/repos/client/gnunet
IH=$ROOT/repos/injector/gnunet

export CLIENT_CFG=$CH/peer.conf
export INJECTOR_CFG=$IH/peer.conf

rm -rf $ROOT/repos/client/gnunet/.local/share/gnunet/peerinfo/hosts
rm -rf $ROOT/repos/injector/gnunet/.local/share/gnunet/peerinfo/hosts

trap "pkill 'gnunet-*' -9 || true" INT EXIT

GNUNET_TEST_HOME=$CH gnunet-arm -s -c $CLIENT_CFG &
GNUNET_TEST_HOME=$IH gnunet-arm -s -c $INJECTOR_CFG &

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
