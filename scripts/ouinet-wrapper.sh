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
LIBS=/opt/ouinet/lib
REPO=$(dirname $CONF)

repo_arg=$(get_repo_from_args "$@")
REPO="${repo_arg:-$REPO}"
CACHE_STATIC_ROOT=/var/opt/ouinet-static-cache

CLIENT_PROXY_PORT=8077
INJECTOR_LOOP_ADDR=127.7.2.1
INJECTOR_TCP_PORT=7070
INJECTOR_TCP_TLS_PORT=7077
INJECTOR_UTP_TLS_PORT=7085

# Fix some configuration parameters on repo creation.
if [ ! -d "$REPO" ] && ! has_help_arg "$@"; then
    cp -r "$INST/repo-templates/$PROG" "$REPO"
    chmod o-rwx "$REPO"

    # Set a well-known client HTTP proxy port.
    if [ "$PROG" = client ]; then
        sed -i -E "s/^(listen-on-tcp\s*=\s*.*:)[0-9]+(.*)/\1${CLIENT_PROXY_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector loopback TCP port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tcp\s*=\s*)127\.0\.0\.1:[0-9]+(.*)/\1${INJECTOR_LOOP_ADDR}:${INJECTOR_TCP_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector TCP/TLS port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tcp-tls\s*=\s*:::)[0-9]+(.*)/\1${INJECTOR_TCP_TLS_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector UTP/TLS port (and enable it).
    # IPv6 is not yet properly supported, listen only on IPv4.
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-utp-tls\s*=\s*0\.0\.0\.0:)[0-9]+(.*)/\1${INJECTOR_UTP_TLS_PORT}\2/" "$CONF"
    fi

    # Generate a random password for injector credentials.
    if [ "$PROG" = injector ]; then
        password=$(dd if=/dev/urandom bs=1024 count=1 status=none | md5sum | cut -f1 -d' ')
        sed -i -E "s/^(credentials\s*=\s*).*/\1ouinet:$password/" "$CONF"
    fi
fi

# Enable features depending on the environment.
if [ "$PROG" = client ] && ! has_help_arg "$@"; then
    # Enable the static cache if present (and not yet done).
    if test -d "$CACHE_STATIC_ROOT/.ouinet" && ! grep -qE '^#*\s*cache-static-root\s*=' "$CONF"; then
        echo "cache-static-root = $CACHE_STATIC_ROOT" >> "$CONF"
    fi
fi

if [ "$OUINET_DEBUG" = yes ]; then
    run() {
        LD_LIBRARY_PATH="$LIBS" \
        exec gdb -return-child-result -batch \
                 -ex='handle SIGPIPE nostop noprint pass' -ex=r -ex=bt \
                 --args "$INST/$PROG" "$@"
    }
else
    run() {
        LD_LIBRARY_PATH="$LIBS" \
        exec "$INST/$PROG" "$@"
    }
fi

# Prepend the installation path to use supporting binaries in there.
export PATH="$INST:$PATH"
if [ "$repo_arg" ]; then
    run "$@"
else
    run --repo "$REPO" "$@"
fi
