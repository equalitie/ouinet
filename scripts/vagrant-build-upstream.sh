#!/bin/sh

#
# TO BE RAN INSIDE THE VAGRANT INSTANCE DEFINED IN ../Vagrantfile
#

#
# Builds ouinet from the upstream github sources.
#

set -e

cd
rm -rf ouinet-upstream
mkdir ouinet-upstream
cd ouinet-upstream
bash /vagrant/scripts/build-ouinet.sh
cd ..
