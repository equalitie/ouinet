#!/bin/bash

CMAKE_VERSION=3.7.2
GMP_VERSION=6.1.2
MPFR_VERSION=3.1.5
MPC_VERSION=1.0.3
BINUTILS_VERSION=2.28.1
GCC_VERSION=6.3.0
MINGW_VERSION=5.0.1
BOOST_VERSION=1.65.1
ZLIB_VERSION=1.2.11
OPENSSL_VERSION=1.1.1-pre2

CMAKE_DIR=cmake-${CMAKE_VERSION}
CMAKE_SOURCE=${CMAKE_DIR}.tar.gz
CMAKE_MIRROR=https://cmake.org/files/v${CMAKE_VERSION:0:3}/${CMAKE_SOURCE}

GMP_DIR=gmp-${GMP_VERSION}
GMP_SOURCE=${GMP_DIR}.tar.bz2
GMP_MIRROR=http://ftp.gnu.org/gnu/gmp/${GMP_SOURCE}

MPFR_DIR=mpfr-${MPFR_VERSION}
MPFR_SOURCE=${MPFR_DIR}.tar.bz2
MPFR_MIRROR=http://ftp.gnu.org/gnu/mpfr/${MPFR_SOURCE}

MPC_DIR=mpc-${MPC_VERSION}
MPC_SOURCE=${MPC_DIR}.tar.gz
MPC_MIRROR=http://ftp.gnu.org/gnu/mpc/${MPC_SOURCE}

BINUTILS_DIR=binutils-${BINUTILS_VERSION}
BINUTILS_SOURCE=${BINUTILS_DIR}.tar.bz2
BINUTILS_MIRROR=http://ftp.gnu.org/gnu/binutils/${BINUTILS_SOURCE}

GCC_DIR=gcc-${GCC_VERSION}
GCC_SOURCE=${GCC_DIR}.tar.bz2
GCC_MIRROR=http://ftp.gnu.org/gnu/gcc/${GCC_DIR}/${GCC_SOURCE}

MINGW_DIR=mingw-w64-v${MINGW_VERSION}
MINGW_SOURCE=${MINGW_DIR}.tar.bz2
MINGW_MIRROR=http://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/${MINGW_SOURCE}

BOOST_DIR=boost_${BOOST_VERSION//./_}
BOOST_SOURCE=${BOOST_DIR}.tar.bz2
BOOST_MIRROR=http://sourceforge.net/projects/boost/files/boost/${BOOST_VERSION}/${BOOST_SOURCE}

ZLIB_DIR=zlib-${ZLIB_VERSION}
ZLIB_SOURCE=${ZLIB_DIR}.tar.gz
ZLIB_MIRROR=https://zlib.net/${ZLIB_SOURCE}

OPENSSL_DIR=openssl-${OPENSSL_VERSION}
OPENSSL_SOURCE=${OPENSSL_DIR}.tar.gz
OPENSSL_MIRROR=https://www.openssl.org/source/${OPENSSL_SOURCE}



fetch() {
	mkdir -p "${SOURCEDIR}"
	cd "${SOURCEDIR}"
	[[ -e $(basename $1) ]] || wget "$1"
	[[ -e $2 ]] || tar xf "$(basename $1)"
}



build_cmake() {
	[[ -e "${TARGETSDIR}"/native/bin/cmake ]] && return
	fetch ${CMAKE_MIRROR} ${CMAKE_DIR}
	rm -rf "${TEMPDIR}"/cmake
	mkdir -p "${TEMPDIR}"/cmake
	cd "${TEMPDIR}"/cmake
	"${SOURCEDIR}"/${CMAKE_DIR}/configure --prefix="${TARGETSDIR}"/native
	make ${MAKEOPTS}
	make install
}

build_gmp() {
	[[ -e "${TARGETSDIR}"/native/include/gmp.h ]] && return
	fetch ${GMP_MIRROR} ${GMP_DIR}
	rm -rf "${TEMPDIR}"/gmp
	mkdir -p "${TEMPDIR}"/gmp
	cd "${TEMPDIR}"/gmp
	"${SOURCEDIR}"/${GMP_DIR}/configure \
		--prefix="${TARGETSDIR}"/native \
		--disable-shared \
		--enable-static
	make ${MAKEOPTS}
	make install
}

build_mpfr() {
	[[ -e "${TARGETSDIR}"/native/include/mpfr.h ]] && return
	fetch ${MPFR_MIRROR} ${MPFR_DIR}
	rm -rf "${TEMPDIR}"/mpfr
	mkdir -p "${TEMPDIR}"/mpfr
	cd "${TEMPDIR}"/mpfr
	"${SOURCEDIR}"/${MPFR_DIR}/configure \
		--prefix="${TARGETSDIR}"/native \
		--disable-shared \
		--enable-static \
		--with-gmp="${TARGETSDIR}"/native
	make ${MAKEOPTS}
	make install
}

build_mpc() {
	[[ -e "${TARGETSDIR}"/native/include/mpc.h ]] && return
	fetch ${MPC_MIRROR} ${MPC_DIR}
	rm -rf "${TEMPDIR}"/mpc
	mkdir -p "${TEMPDIR}"/mpc
	cd "${TEMPDIR}"/mpc
	"${SOURCEDIR}"/${MPC_DIR}/configure \
		--prefix="${TARGETSDIR}"/native \
		--disable-shared \
		--enable-static \
		--with-gmp="${TARGETSDIR}"/native \
		--with-mpfr="${TARGETSDIR}"/native
	make ${MAKEOPTS}
	make install
}

build_general() {
	build_cmake
	build_gmp
	build_mpfr
	build_mpc
}

set_toolchain() {
	TARGET=$1
	shift
	EXTRA_LD_LIBRARY_PATH=
	for dir in lib64 lib32 lib; do
		if [[ -e "${TARGETSDIR}"/native/${dir}/libstdc++.so ]]; then
			EXTRA_LD_LIBRARY_PATH="${TARGETSDIR}/native/${dir}:${EXTRA_LD_LIBRARY_PATH}"
		fi
	done
	PATH="${TARGETSDIR}/${TARGET}/bin:${TARGETSDIR}/native/bin:${PATH}" \
	LD_LIBRARY_PATH="${EXTRA_LD_LIBRARY_PATH}${LD_LIBRARY_PATH}" \
	"$@"
}



build_mingw_headers() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/include/windows.h ]] && return
	fetch ${MINGW_MIRROR} ${MINGW_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-mingw-headers
	mkdir -p "${TEMPDIR}"/${TARGET}-mingw-headers
	cd "${TEMPDIR}"/${TARGET}-mingw-headers
	"${SOURCEDIR}"/${MINGW_DIR}/mingw-w64-headers/configure \
		--host=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET}/sysroot/mingw
	make ${MAKEOPTS}
	make install
}

build_mingw_runtime() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/libkernel32.a ]] && return
	fetch ${MINGW_MIRROR} ${MINGW_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-mingw-runtime
	mkdir -p "${TEMPDIR}"/${TARGET}-mingw-runtime
	cd "${TEMPDIR}"/${TARGET}-mingw-runtime
	"${SOURCEDIR}"/${MINGW_DIR}/mingw-w64-crt/configure \
		--host=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET}/sysroot/mingw
	make ${MAKEOPTS}
	make install
	
	rm -rf "${TEMPDIR}"/${TARGET}-mingw-pthread
	mkdir -p "${TEMPDIR}"/${TARGET}-mingw-pthread
	cd "${TEMPDIR}"/${TARGET}-mingw-pthread
	"${SOURCEDIR}"/${MINGW_DIR}/mingw-w64-libraries/winpthreads/configure \
		--host=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET}/sysroot/mingw
	make ${MAKEOPTS}
	make install
}

build_binutils_windows() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/bin/${TARGET}-ld ]] && return
	fetch ${BINUTILS_MIRROR} ${BINUTILS_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-binutils
	mkdir -p "${TEMPDIR}"/${TARGET}-binutils
	cd "${TEMPDIR}"/${TARGET}-binutils
	"${SOURCEDIR}"/${BINUTILS_DIR}/configure \
		--target=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET} \
		--with-sysroot="${TARGETSDIR}"/${TARGET}/sysroot
	make ${MAKEOPTS}
	make install
}

build_gcc_windows_stage1() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/bin/${TARGET}-g++ ]] && return
	fetch ${GCC_MIRROR} ${GCC_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-gcc-stage1
	mkdir -p "${TEMPDIR}"/${TARGET}-gcc-stage1
	cd "${TEMPDIR}"/${TARGET}-gcc-stage1
	"${SOURCEDIR}"/${GCC_DIR}/configure \
		--target=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET} \
		--with-gmp="${TARGETSDIR}"/native \
		--with-mpfr="${TARGETSDIR}"/native \
		--with-mpc="${TARGETSDIR}"/native \
		--enable-languages=c,c++ \
		--disable-multilib \
		--with-sysroot="${TARGETSDIR}"/${TARGET}/sysroot \
		--enable-threads=posix \
		--disable-win32-registry
	make all-gcc ${MAKEOPTS}
	make install-gcc
}

build_gcc_windows_stage2() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/lib/gcc/${TARGET}/${GCC_VERSION}/libgcc.a ]] && return
	fetch ${GCC_MIRROR} ${GCC_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-gcc-stage2
	mkdir -p "${TEMPDIR}"/${TARGET}-gcc-stage2
	cd "${TEMPDIR}"/${TARGET}-gcc-stage2
	"${SOURCEDIR}"/${GCC_DIR}/configure \
		--target=${TARGET} \
		--prefix="${TARGETSDIR}"/${TARGET} \
		--with-gmp="${TARGETSDIR}"/native \
		--with-mpfr="${TARGETSDIR}"/native \
		--with-mpc="${TARGETSDIR}"/native \
		--enable-languages=c,c++ \
		--disable-multilib \
		--with-sysroot="${TARGETSDIR}"/${TARGET}/sysroot \
		--enable-threads=posix \
		--disable-win32-registry
	make ${MAKEOPTS}
	make install
}

build_toolchain_windows() {
	set_toolchain $1 build_mingw_headers $1
	set_toolchain $1 build_binutils_windows $1
	set_toolchain $1 build_gcc_windows_stage1 $1
	set_toolchain $1 build_mingw_runtime $1
	set_toolchain $1 build_gcc_windows_stage2 $1
}



build_zlib() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/libz.a ]] && return
	fetch ${ZLIB_MIRROR} ${ZLIB_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-zlib
	cp -a "${SOURCEDIR}"/${ZLIB_DIR} "${TEMPDIR}"/${TARGET}-zlib
	cd "${TEMPDIR}"/${TARGET}-zlib
	
	case ${TARGET} in
	*-w64-*)
		:
		;;
	*)
		echo "Unsupported platform for zlib"
		exit 1
		;;
	esac
	
	make -f win32/Makefile.gcc \
		BINARY_PATH="${TARGETSDIR}"/${TARGET}/sysroot/mingw/bin \
		INCLUDE_PATH="${TARGETSDIR}"/${TARGET}/sysroot/mingw/include \
		LIBRARY_PATH="${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib \
		PREFIX="${TARGET}-" \
		install \
		${MAKEOPTS}
}

build_boost() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/libboost_coroutine.dll ]] && return
	fetch ${BOOST_MIRROR} ${BOOST_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-boost
	cp -a "${SOURCEDIR}"/${BOOST_DIR} "${TEMPDIR}"/${TARGET}-boost
	cd "${TEMPDIR}"/${TARGET}-boost
	
	./bootstrap.sh
	
	OPTIONS=
	case ${TARGET} in
	i686-w64-*)
		echo "using gcc : mingw : ${TARGET}-g++ ;" > user-config.jam
		OPTIONS="${OPTIONS} \
			toolset=gcc-mingw \
			target-os=windows \
			link=shared \
			address-model=32 \
			architecture=x86 \
			binary-format=pe \
			abi=ms \
			threadapi=win32 \
			threading=multi
		"
		;;
	x86_64-w64-*)
		echo "using gcc : mingw : ${TARGET}-g++ ;" > user-config.jam
		OPTIONS="${OPTIONS} \
			toolset=gcc-mingw \
			target-os=windows \
			link=shared \
			address-model=64 \
			architecture=x86 \
			binary-format=pe \
			abi=ms \
			threadapi=win32 \
			threading=multi
		"
		;;
	*)
		echo "Unsupported platform for boost"
		exit 1
		;;
	esac
	
	./b2 \
		--ignore-site-config \
		--user-config=user-config.jam \
		--with-system \
		--with-program_options \
		--with-test \
		--with-coroutine \
		--with-filesystem \
		--with-date_time \
		--with-regex \
		--with-thread \
		install \
		--prefix="${TARGETSDIR}"/${TARGET}/sysroot/mingw \
		variant=release \
		cxxflags="-std=gnu++11" \
		${OPTIONS} \
		${MAKEOPTS}
	
	case ${TARGET} in
	*-w64-*)
		mv "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/{libboost_thread_win32.dll,libboost_thread.dll}
		mv "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/{libboost_thread_win32.dll.a,libboost_thread.dll.a}
		;;
	esac
}

build_openssl() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/lib/libssl.dll.a ]] && return
	fetch ${OPENSSL_MIRROR} ${OPENSSL_DIR}
	rm -rf "${TEMPDIR}"/${TARGET}-openssl
	cp -a "${SOURCEDIR}"/${OPENSSL_DIR} "${TEMPDIR}"/${TARGET}-openssl
	cd "${TEMPDIR}"/${TARGET}-openssl
	
	case ${TARGET} in
	i686-w64-*)
		local TOOLCHAIN=mingw
		;;
	x86_64-w64-*)
		local TOOLCHAIN=mingw64
		;;
	*)
		echo "Unsupported platform for openssl"
		exit 1
		;;
	esac
	
	CC="${TARGET}-gcc" \
	RC="${TARGET}-windres" \
	./Configure \
		${TOOLCHAIN} \
		zlib \
		shared \
		no-capieng \
		--prefix="${TARGETSDIR}"/${TARGET}/sysroot/mingw
	
	make \
		CC="${TARGET}-gcc" \
		AR="${TARGET}-ar" \
		RANLIB="${TARGET}-ranlib" \
		RC="${TARGET}-windres" \
		CROSS_COMPILE="${TARGET}-" \
		all \
		${MAKEOPTS}
	
	make install_sw
	
	case ${TARGET} in
	*-w64-*)
		mv "${TARGETSDIR}"/${TARGET}/sysroot/mingw{/bin/libcrypto-*.dll,/bin/libssl-*.dll,/lib}
		;;
	esac
}

build_target_dependencies() {
	set_toolchain $1 build_zlib $1
	set_toolchain $1 build_boost $1
	set_toolchain $1 build_openssl $1
}



do_cmake() {
	TARGET=$1
	shift
	
	if [[ ${TARGET} == "native" ]]; then
		cmake \
			-DCMAKE_C_COMPILER=gcc \
			-DCMAKE_CXX_COMPILER=g++ \
			-DCMAKE_FIND_ROOT_PATH="${TARGETSDIR}"/${TARGET} \
			-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH \
			-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
			-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
			"$@"
	else
		case ${TARGET} in
		*-w64-*)
			SYSTEM_NAME=Windows
			;;
		*-linux-*)
			SYSTEM_NAME=Linux
			;;
		*-darwin*)
			SYSTEM_NAME=Darwin
			;;
		esac
		
		cmake \
			-DCMAKE_SYSTEM_NAME=${SYSTEM_NAME} \
			-DCMAKE_SYSTEM_PROCESSOR=${TARGET%%-*} \
			-DCMAKE_C_COMPILER=${TARGET}-gcc \
			-DCMAKE_CXX_COMPILER=${TARGET}-g++ \
			-DCMAKE_FIND_ROOT_PATH="${TARGETSDIR}"/${TARGET}/sysroot/mingw \
			-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
			-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
			-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
			"$@"
	fi
}

build_ouinet() {
	TARGET=$1
	[[ -e "${TARGETSDIR}"/${TARGET}/sysroot/mingw/bin/injector.exe ]] && return
	rm -rf "${TEMPDIR}"/${TARGET}-ouinet
	mkdir "${TEMPDIR}"/${TARGET}-ouinet
	cd "${TEMPDIR}"/${TARGET}-ouinet
	
	#
	# Some build script calls windres rather than ${TARGET}-windres and I don't feel like tracking it down
	#
	echo -e "#!/bin/bash\n${TARGET}-windres \"\$@\"" > "${TARGETSDIR}"/${TARGET}/bin/windres
	chmod 755 "${TARGETSDIR}"/${TARGET}/bin/windres
	
	
	do_cmake ${TARGET} "${OUINET}"
	make VERBOSE=1 #${MAKEOPTS}
	
	
}

build_target_ouinet() {
	set_toolchain $1 build_ouinet $1
}



if [[ $# != 2 ]]; then
	echo "Usage: build-ouinet-windows.sh <build directory> <ouinet source directory>"
	exit 1
fi

set -e

current=$(pwd)

mkdir -p "$1"
cd "$1"
export BUILDDIR=$(pwd)
cd "${current}"

cd "$2"
export OUINET=$(pwd)
cd "${current}"

export SOURCEDIR="${BUILDDIR}/source"
export TARGETSDIR="${BUILDDIR}/targets"
export TEMPDIR="${BUILDDIR}/temp"

export CFLAGS="-O2 -pipe"
export CXXFLAGS="${CFLAGS}"

mkdir -p "${SOURCEDIR}" "${TARGETSDIR}" "${TEMPDIR}"

build_general
build_toolchain_windows i686-w64-mingw32
build_toolchain_windows x86_64-w64-mingw32

build_target_dependencies i686-w64-mingw32
build_target_dependencies x86_64-w64-mingw32

build_target_ouinet x86_64-w64-mingw32
