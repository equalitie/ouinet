#!/bin/sh

#
# Builds or updates the ouinet software.
#

#
# Major TODOs:
# - separate the build directory from the install directory
# - move the building of golang out of ipfs-cache and into this script
# - enable use-this-script support
# - manage configs, including things like keys and addresses
# - support parallel builds without intermittent errors
#

set -e

FORCE=false
BRANCH=master

#USE_THIS_SCRIPT=false

print_help() {
	echo "Usage: $0 [options]"
	echo "Options:"
	echo "  --help                         # Show this help"
	echo "  --branch=<branch-name>         # Select a specific ouinet branch"
#	echo "  --use-this-script              # Don't download the newest version of this script, but use this one"
	echo "  --force                        # Ignore missing dependencies"
}

for i in "$@"; do
	case $i in
		-b=*|--branch=*)
			BRANCH="${i#*=}"
			;;
#		-u|--use-this-script)
#			USE_THIS_SCRIPT=true
#			;;
		--force)
			FORCE=true
			;;
		-h|--help)
			print_help
			exit
			;;
		*)
			echo "Error: unknown option \"${i}\""
			print_help
			exit
			;;
	esac
done



testheader() {
	RETURN=0
	echo "#include <$1>\nmain(){}" > conftest.c
	${CXX-c++} conftest.c -c -o conftest.o >/dev/null 2>/dev/null || RETURN=1
	rm -f conftest.c conftest.o
	return $RETURN
}

testlib() {
	RETURN=0
	echo "main(){}" > conftest.c
	${CXX-c++} conftest.c -o conftest.a.out -l"$1" >/dev/null 2>/dev/null || RETURN=1
	rm -f conftest.c conftest.a.out
	return $RETURN
}

testcxxversion() {
	RETURN=0
	echo "main(){}" > conftest.c
	${CXX-c++} -std="$1" conftest.c -o conftest.a.out >/dev/null 2>/dev/null || RETURN=1
	rm -f conftest.c conftest.a.out
	return $RETURN
}

testasan() {
	RETURN=0
	echo "main(){}" > conftest.c
	${CXX-c++} conftest.c -fsanitize=address -o conftest.a.out >/dev/null 2>/dev/null || RETURN=1
	rm -f conftest.c conftest.a.out
	return $RETURN
}



WORKDIR="`pwd`"



DEPS=""

type ${CC-cc} >/dev/null 2>/dev/null || DEPS="${DEPS} c-compiler"
type ${CXX-c++} >/dev/null 2>/dev/null || DEPS="${DEPS} cxx-compiler"
type pkg-config >/dev/null 2>/dev/null || DEPS="${DEPS} pkg-config"

if [ -n "${DEPS}" ]; then
	echo "Missing dependencies: ${DEPS}"
	${FORCE} || echo "Ignore this warning with --force."
	${FORCE} || exit 1
	echo "Ignoring missing dependencies."
fi

testcxxversion "c++14" || DEPS="${DEPS} c++14-support"
type git >/dev/null 2>/dev/null || DEPS="${DEPS} git"
type wget >/dev/null 2>/dev/null || DEPS="${DEPS} wget"
type cmake >/dev/null 2>/dev/null || DEPS="${DEPS} cmake"
type libtoolize >/dev/null 2>/dev/null || DEPS="${DEPS} libtool"
type autoconf >/dev/null 2>/dev/null || DEPS="${DEPS} autoconf"
type automake >/dev/null 2>/dev/null || DEPS="${DEPS} automake"
type autopoint >/dev/null 2>/dev/null || DEPS="${DEPS} autopoint"
type makeinfo >/dev/null 2>/dev/null || DEPS="${DEPS} texinfo"
testheader boost/asio.hpp || DEPS="${DEPS} libboost-dev"
testlib boost_coroutine || DEPS="${DEPS} libboost-coroutine-dev"
testlib boost_program_options || DEPS="${DEPS} libboost-program-options-dev"
testlib boost_system || DEPS="${DEPS} libboost-system-dev"
testlib boost_unit_test_framework || DEPS="${DEPS} libboost-test-dev"
testlib boost_thread || DEPS="${DEPS} libboost-thread-dev"
testlib boost_filesystem || DEPS="${DEPS} libboost-filesystem-dev"
testheader gcrypt.h || DEPS="${DEPS} libgcrypt-dev"
testheader idna.h || DEPS="${DEPS} libidn11-dev"
testheader unistr.h || DEPS="${DEPS} libunistring-dev"
testheader zlib.h || DEPS="${DEPS} zlib1g-dev"

if [ -n "${DEPS}" ]; then
	echo "Missing dependencies: ${DEPS}"
	${FORCE} || echo "Ignore this warning with --force."
	${FORCE} || exit 1
	echo "Ignoring missing dependencies."
fi



if testasan; then
	export CFLAGS="${CFLAGS} -fsanitize=address -ggdb"
	export CXXFLAGS="${CXXFLAGS} -fsanitize=address -ggdb"
fi



if [ ! -d ouinet ]; then
	rm -rf ouinet ouinet-build
	git clone https://github.com/equalitie/ouinet.git ouinet
	cd ouinet
	git checkout "${BRANCH}"
	git submodule update --init --recursive --checkout
	cd ..
	mkdir ouinet-build
	cd ouinet-build
	cmake ../ouinet -DCMAKE_INSTALL_PREFIX="${WORKDIR}"/bin -DCMAKE_BUILD_TYPE=Debug
	make #${MAKEOPTS}
	# make install
	cd ..
else
	cd ouinet
	git remote update
	if [ `git rev-parse @` != `git rev-parse origin/"${BRANCH}"` ]; then
		git checkout "${BRANCH}"
		git pull origin "${BRANCH}"
		git submodule update --init --recursive --checkout
		cd ..
		cd ouinet-build
		make #${MAKEOPTS}
		# make install
	fi
	cd ..
fi

if [ ! -d ouinet-repos ]; then
	cp -r ouinet/repos ouinet-repos
fi

echo "\
Now run:
  env BUILD=ouinet-build REPOS=ouinet-repos ouinet/scripts/start-gnunet-services.sh client injector &
Then:
  ouinet-build/injector --repo ouinet-repos/injector <other parameters>
Or:
  ouinet-build/client --repo ouinet-repos/client <other parameters>
"
