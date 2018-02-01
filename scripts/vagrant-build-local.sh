#!/bin/sh

#
# TO BE RAN INSIDE THE VAGRANT INSTANCE DEFINED IN ../Vagrantfile
#

#
# Builds ouinet from the local sources mounted in /vagrant
#

set -e

NPROC=$(lscpu | grep '^CPU(s):' | awk '{ print $2 }')

cd
DIR=$(pwd)
rm -rf ouinet-local-build ouinet-local-bin
mkdir ouinet-local-build ouinet-local-bin
cd ouinet-local-build

cmake /vagrant -DCMAKE_INSTALL_PREFIX="${DIR}"/ouinet-local-bin -DCMAKE_BUILD_TYPE=Debug
# Parallel build disabled until it is debugged
make #-j ${NPROC}

# Not supported yet
#make install

cd ..
