#!/bin/bash

#
# Builds ouinet from the local source containing this file
#
# If BOOST_ROOT env variable is set it adds -DBOOST_ROOT=$BOOST_ROOT
# to cmake args

set -e

NPROC=$(lscpu | grep '^CPU(s):' | awk '{ print $2 }')

DIR=$(pwd)
SOURCEDIR=$(dirname "$(realpath "${BASH_SOURCE}")")/..
BINDIR="${DIR}"/ouinet-local-bin
BUILDDIR="${DIR}"/ouinet-local-build

if [[ ! -e ${BUILDDIR}/Makefile ]]; then
	rm -rf "${BUILDDIR}"
	mkdir "${BUILDDIR}"
	cd "${BUILDDIR}"
    CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=\"${BINDIR}\""
    [ ! -z "$BOOST_ROOT" ] && CMAKE_ARGS=$CMAKE_ARGS" -DBOOST_ROOT=$BOOST_ROOT"
	cmake "${SOURCEDIR}" $CMAKE_ARGS
fi

cd "${BUILDDIR}"
# Parallel build disabled until it is debugged
make #-j${NPROC}

# Not supported yet
#make install

cd "${DIR}"
