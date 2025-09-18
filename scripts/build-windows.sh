#!/bin/bash

#
# Builds ouinet from the local source containing this file
#
# If BOOST_ROOT env variable is set it adds -DBOOST_ROOT=$BOOST_ROOT
# to cmake args

set -e
BUILDTYPE=${BUILDTYPE:-Debug}

native_source_dir=$( dirname "$(realpath "${BASH_SOURCE}")" )/..
SOURCEDIR=$( cygpath -u $(realpath ${native_source_dir} ))

BASEDIR=${SOURCEDIR}".build"
BUILDDIR="${BASEDIR}"/ouinet-windows-build

OUIVERSION=$(cat ${SOURCEDIR}/version.txt)
DISTNAME="ouinet-windows-x64-v${OUIVERSION}"
BINDIR="${BASEDIR}"/${DISTNAME}

SIGNTOOL=/c/Program\ Files\ \(x86\)/Windows\ Kits/10/bin/10.0.26100.0/x64/signtool.exe
MSYS2DIR=/c/msys64/mingw64/bin


echo $BUILDDIR
GENERATOR="Unix Makefiles"


if [[ ! -e ${BUILDDIR}/Makefile ]]; then
	rm -rf "${BUILDDIR}"
	mkdir -p "${BUILDDIR}"
	cd "${BUILDDIR}"
	cmake \
    "${SOURCEDIR}" \
    -G "${GENERATOR}" \
    -DCMAKE_BUILD_TYPE=${BUILDTYPE}
fi

cmake \
  --build "${BUILDDIR}" \
  -- \
  -j`nproc`

if [[ $BUILDTYPE == "Release" ]]; then
  echo
  echo "Copying EXE and DLL files to ${BINDIR}"
  mkdir -p ${BINDIR}
  cp ${BUILDDIR}/*.exe ${BINDIR}
  cp ${BUILDDIR}/*lib* ${BINDIR}
  cp ${BUILDDIR}/{gcrypt,gpg_error}/out/{lib,bin}/*dll* ${BINDIR}
  cp ${MSYS2DIR}/{"libgcc_s_seh-1","libstdc++-6","libwinpthread-1","zlib1"}.dll ${BINDIR}

  echo
  echo "Signing EXEs"
  "${SIGNTOOL}" sign //a \
                 //fd SHA256 \
                 //tr http://timestamp.digicert.com \
                 //td SHA256 \
                 ${BINDIR}/*exe

  echo
  echo "Compressing into ${BINDIR}.zip"
  zip ${DISTNAME}.zip ./${DISTNAME}/*

  echo
  echo "Calculating sha256 sums ${DISTNAME}.sha256sums"
  sha256sum ${DISTNAME}/* > ${DISTNAME}.sha256sums

fi

# Not supported yet
#make install

cd "${BASEDIR}"
