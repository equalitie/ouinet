#!/bin/sh
# Populate repo and start the injector or client.
# Both program binary and repos templates should be in the directory
# where this wrapper script resides.

set -e

has_help_arg() {
    for arg in "$@"; do
        if [ "$arg" = --help ]; then
            return 0
        fi
    done
    return 1
}

# Checks for args overriding the default repository.
get_repo_from_args() {
    local arg setnext=n repo
    for arg in "$@"; do
        if [ "$setnext" = y ]; then
            repo="$arg"  # previous arg was "--repo"
            setnext=n
            continue
        fi
        case "$arg" in
            (--repo=*)
                repo="${arg#--repo=}"
                ;;
            (--repo)
                setnext=y
                ;;
            (*) ;;
        esac
    done
    echo "$repo"
}

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

repo_arg=$(get_repo_from_args "$@")
REPO="${repo_arg:-$REPO}"

if [ ! -d "$REPO" ] && ! has_help_arg "$@"; then
    cp -r "$INST/repo-templates/$PROG" "$REPO"
fi

if [ "$repo_arg" ]; then
    exec "$INST/$PROG" "$@"
else
    exec "$INST/$PROG" --repo "$REPO" "$@"
fi
