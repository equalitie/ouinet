#!/bin/bash

#
# Builds ouinet from the local source containing this file
#
# If BOOST_ROOT env variable is set it adds -DBOOST_ROOT=$BOOST_ROOT
# to cmake args

set -e

native_source_dir=$( dirname "$(realpath "${BASH_SOURCE}")" )/..
SOURCEDIR=$( cygpath -u $(realpath ${native_source_dir} ))

BASEDIR=${SOURCEDIR}".build"

BINDIR="${BASEDIR}"/ouinet-windows-bin
BUILDDIR="${BASEDIR}"/ouinet-windows-build
GENERATOR="Unix Makefiles"


if [[ ! -e ${BUILDDIR}/Makefile ]]; then
	rm -rf "${BUILDDIR}"
	mkdir "${BUILDDIR}"
	cd "${BUILDDIR}"
	cmake \
    "${SOURCEDIR}" \
    -G "${GENERATOR}" \
    -DCMAKE_BUILD_TYPE=Debug
fi

cmake \
  --build "${BUILDDIR}" \
  -t client \
  -- \
  -j`nproc`

# Not supported yet
#make install

cd "${BASEDIR}"
