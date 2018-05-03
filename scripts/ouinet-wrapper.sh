#!/bin/sh
# Populate repo and start the injector or client.
# Both program binary and repos templates should be in the directory
# where this wrapper script resides.

set -e

case "$1" in
    (injector|client) ;;
    (*) echo "Please specify whether to run the injector or the client." >&2
        exit 1
        ;;
esac

INST="$(dirname -- "$(readlink -f -- "$0")")"
PROG=$1

CONF=/var/opt/ouinet/$PROG/ouinet-$PROG.conf
REPO=$(dirname $CONF)

if [ ! -d "$REPO" ]; then
    cp -r "$INST/repo-templates/$PROG" "$REPO"
fi

exec "$INST/$PROG" --repo "$REPO" "$@"
