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

LOCAL_ADDR=127.7.2.1
CLIENT_PROXY_PORT=8080
INJECTOR_TLS_PORT=7077

if [ ! -d "$REPO" ] && ! has_help_arg "$@"; then
    cp -r "$INST/repo-templates/$PROG" "$REPO"

    # Fix local listening addresses to a well-known, identifiable value
    # that does (hopefully) not clash with other daemons.
    sed -i -E "s/^(listen-on-\S+\s*=\s*)127\.0\.0\.1(:.*)/\1${LOCAL_ADDR}\2/" "$CONF"

    # Set a well-known client HTTP proxy port.
    if [ "$PROG" = client ]; then
        sed -i -E "s/^(listen-on-tcp\s*=\s*${LOCAL_ADDR}:)[0-9]+(.*)/\1${CLIENT_PROXY_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector TLS port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tls\s*=\s*:::)[0-9]+(.*)/\1${INJECTOR_TLS_PORT}\2/" "$CONF"
    fi
fi

if [ "$OUINET_DEBUG" = yes ]; then
    run() {
        exec gdb -return-child-result -batch \
                 -ex='handle SIGPIPE nostop noprint pass' -ex=r -ex=bt \
                 --args "$INST/$PROG" "$@"
    }
else
    run() {
        exec "$INST/$PROG" "$@"
    }
fi

if [ "$repo_arg" ]; then
    run "$@"
else
    run --repo "$REPO" "$@"
fi
