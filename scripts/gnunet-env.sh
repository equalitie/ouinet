#!/bin/bash

# Setup GNUnet environment

export SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")

# Note, this script assumes that you built `ouinet` in the
# `${CMAKE_SOURCE_DIR}/build` directory and that client and injector repos sit
# under `${CMAKE_SOURCE_DIR}/repos`. If not, modify the BUILD and REPOS
# variables below.

ROOT=$SCRIPT_DIR/..
BUILD=${BUILD:-$ROOT/build}
REPOS=${REPOS:-$ROOT/repos}

GNUNET_ROOT=$BUILD/modules/gnunet-channels/gnunet-bin

export PATH=$GNUNET_ROOT/bin:$PATH

CLIENT_HOME=$REPOS/client/gnunet
INJECTOR_HOME=$REPOS/injector/gnunet

CLIENT_CFG=$CLIENT_HOME/peer.conf
INJECTOR_CFG=$INJECTOR_HOME/peer.conf

function get_id {
	gnunet-peerinfo -s -c $1 | sed "s/I am peer \`\(.*\)'\./\1/"
}

function client_id {
	get_id $CLIENT_CFG
}

function injector_id {
	get_id $INJECTOR_CFG
}
