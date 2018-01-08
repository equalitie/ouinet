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

CLIENT_CFG=$CLIENT_HOME/peer.conf
INJECTOR_CFG=$INJECTOR_HOME/peer.conf

rm -rf $CLIENT_HOME/.local/share/gnunet/peerinfo/hosts
rm -rf $INJECTOR_HOME/.local/share/gnunet/peerinfo/hosts

RUN_CLIENT=n
RUN_INJECTOR=n

for arg in "$@"; do
	case $arg in
	client)
		RUN_CLIENT=y
		;;
	injector)
		RUN_INJECTOR=y
		;;
	*)
		echo "Invalid argument \"$arg\""
		;;
	esac
done

if [ $RUN_CLIENT = n -a $RUN_INJECTOR = n ]; then
	RUN_CLIENT=y
	RUN_INJECTOR=y
fi

stop_gnunet() {
    [ $RUN_CLIENT = y ]   && gnunet-arm -e -c $CLIENT_CFG -T 2s
    [ $RUN_INJECTOR = y ] && gnunet-arm -e -c $INJECTOR_CFG -T 2s
}

[ $RUN_CLIENT = y ]   && GNUNET_TEST_HOME=$CLIENT_HOME   gnunet-arm -s -c $CLIENT_CFG
[ $RUN_INJECTOR = y ] && GNUNET_TEST_HOME=$INJECTOR_HOME gnunet-arm -s -c $INJECTOR_CFG

if [ $RUN_CLIENT = y ]; then
	echo "* Client's info"
	gnunet-peerinfo -s -c $CLIENT_CFG
fi

if [ $RUN_INJECTOR = y ]; then
	echo "* Injector's info"
	gnunet-peerinfo -s -c $INJECTOR_CFG
fi

trap stop_gnunet INT EXIT

sleep 1

if [ $RUN_CLIENT = y -a $RUN_INJECTOR = y ]; then
	# This is not necessary, but it makes developing easier/faster.
	echo "Interconnecting the two..."
	gnunet-peerinfo -c $CLIENT_CFG -p `gnunet-peerinfo -c $INJECTOR_CFG -g`
	echo "Done"
fi

# Monitor GNUnet until the user cancels it (any service will do).
gnunet-arm -m -c $CLIENT_CFG
