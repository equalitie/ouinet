#!/bin/sh
# Populate repo and start the injector or client.
# Both program binary and repos templates should be at current directory.

set -e

case "$1" in
    (injector|client) ;;
    (*) echo "Please specify whether to run the injector or the client." >&2
        exit 1
        ;;
esac

PROG=$1

CONF=/var/opt/ouinet/$PROG/ouinet-$PROG.conf
REPO=$(dirname $CONF)

if [ ! -f "$CONF" ]; then
    cp -r repos/$PROG/* "$REPO"
fi

exec ./$PROG --repo "$REPO"
