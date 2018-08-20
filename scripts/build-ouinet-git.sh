#!/bin/bash

#
# Builds ouinet from git upstream
#

set -e

NPROC=$(lscpu | grep '^CPU(s):' | awk '{ print $2 }')

DIR=$(pwd)
SOURCEDIR="${DIR}"/ouinet-git-source
BINDIR="${DIR}"/ouinet-git-bin
BUILDDIR="${DIR}"/ouinet-git-build

: ${BRANCH:=master}

if [[ ! -e ${SOURCEDIR}/CMakeLists.txt ]]; then
	rm -rf "${SOURCEDIR}"
	git clone https://github.com/equalitie/ouinet.git "${SOURCEDIR}"
	cd "${SOURCEDIR}"
	git checkout "${BRANCH}"
else
	cd "${SOURCEDIR}"
	git checkout "${BRANCH}"
	git pull origin "${BRANCH}"
fi
git submodule update --init --recursive --checkout
cd "${DIR}"

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
