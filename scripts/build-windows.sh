#!/bin/bash

#
# Builds ouinet from the local source containing this file
#
# If BOOST_ROOT env variable is set it adds -DBOOST_ROOT=$BOOST_ROOT
# to cmake args

set -e

DIR=$(pwd)
SOURCEDIR=$(dirname "$(realpath "${BASH_SOURCE}")")/..
BINDIR="${DIR}"/ouinet-windows-bin
BUILDDIR="${DIR}"/ouinet-windows-build
GENERATOR="Unix Makefiles"

if [[ ! -e ${BUILDDIR}/Makefile ]]; then
	rm -rf "${BUILDDIR}"
	mkdir "${BUILDDIR}"
	cd "${BUILDDIR}"
	cmake \
    "${SOURCEDIR}" \
    -G "${GENERATOR}" \
    --compile-no-warning-as-error \
    -Wno-error=nonnull \
    -Wno-error=dev \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX="${BINDIR}" \
    -DBOOST_VERSION="1.79.0"
fi

# Using a single threaded compilation to simplify the debugging
cmake \
  --build "${BUILDDIR}" \
  -t client \
  -- \
  -j`nproc`

# Not supported yet
#make install

cd "${DIR}"
