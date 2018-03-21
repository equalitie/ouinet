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

if [ ! -d "$REPO" ]; then
    cp -r repos/$PROG "$REPO"
fi

if [ $PROG = client ]; then
    # Have the client listen on a fixed port in all interfaces
    # so that it can be published to the host.
    sed -i -E 's/^\s*(listen-on-tcp)\b.*/\1 = 0.0.0.0:8080/' "$CONF"
fi

exec ./$PROG --repo "$REPO" "$@"
