#!/bin/bash

#
# Builds ouinet from the local source containing this file
#

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
	cmake "${SOURCEDIR}" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="${BINDIR}"
fi

cd "${BUILDDIR}"
# Parallel build disabled until it is debugged
make #-j${NPROC}

# Not supported yet
#make install

cd "${DIR}"
