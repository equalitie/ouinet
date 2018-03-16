#!/bin/sh
# Populate repo and start the injector.
# Both injector binary and repos templates should be at current directory.

set -e

CONF=/var/opt/ouinet/injector/ouinet-injector.conf
REPO=$(dirname $CONF)

if [ ! -f "$CONF" ]; then
    cp -r repos/injector/* "$REPO"
fi

exec ./injector --repo "$REPO"
