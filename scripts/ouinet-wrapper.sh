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

# Update some renamed configuration parameters.
if grep -qE '^#*\s*(default-index|bittorrent-private-key|bittorrent-public-key|index-bep44-private-key|injector-ipns|listen-in-bep5-swarm|listen-on-tls)\s*=' "$CONF" && ! has_help_arg "$@"; then
    sed -i -E \
        -e 's/^(#*\s*)default-index(\s*=.*)/\1cache-index\2/g' \
        -e 's/^(#*\s*)bittorrent-private-key(\s*=.*)/\1ed25519-private-key\2/g' \
        -e 's/^(#*\s*)bittorrent-public-key(\s*=.*)/\1index-bep44-public-key\2/g' \
        -e 's/^(#*\s*)index-bep44-private-key(\s*=.*)/\1ed25519-private-key\2/g' \
        -e 's/^(#*\s*)injector-ipns(\s*=.*)/\1index-ipns-id\2/g' \
        -e 's/^(#*\s*)listen-in-bep5-swarm(\s*=.*)/\1announce-in-bep5-swarm\2/g' \
        -e 's/^(#*\s*)listen-on-tls(\s*=.*)/\1listen-on-tcp-tls\2/g' \
        "$CONF"
fi

# Update the values of some configuration parameters.
if grep -qP '^\s*injector-ep\s*=(?!\s*[A-Za-z][-\+\.0-9A-Za-z]*:)' "$CONF" && ! has_help_arg "$@"; then
    sed -i -E \
        -e 's/^(\s*injector-ep\s*=\s*)([][\.\:0-9A-Fa-f]+:[0-9]+\s*)(#.*)?$/\1tcp:\2\3/g' \
        -e 's/^(\s*injector-ep\s*=\s*)([2-7A-Za-z]+\.b32\.i2p\s*)(#.*)?$/\1i2p:\2\3/g' \
        -e 's/^(\s*injector-ep\s*=\s*)([-~0-9A-Za-z]+=*\s*)(#.*)?$/\1i2p:\2\3/g' \
        "$CONF"
fi

if grep -qE '^\s*listen-on-utp-tls\s*=\s*\S*:\S*:\S*:[0-9]+' "$CONF" && ! has_help_arg "$@"; then
    sed -i -E \
        -e 's/^(\s*listen-on-utp-tls\s*=\s*)::(:[0-9]+.*)/\10.0.0.0\2  # IPv6 not supported/' \
        -e 's/^(\s*listen-on-utp-tls\s*=\s*\S*:\S*:\S*:[0-9]+.*)/##\1  # IPv6 not supported/' \
        "$CONF"
fi

# Convert completely changed configuration parameters.
if grep -qE '^\s*(debug|disable-cache)\s*=' "$CONF" && ! has_help_arg "$@"; then
    # No Perl regular expressions for sed, bad luck.
    sed -i -E \
        -e 's/^(\s*)debug(\s*=\s*)([Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn]|1|)(\s*)(#.*)?$/\1log-level\2debug\4\5/g' \
        -e 's/^(\s*debug\s*=\s*)([Ff][Aa][Ll][Ss][Ee]|[Nn][Oo]|[Oo][Ff][Ff]|0)(\s*)(#.*)?$/#\1\2\3\4/g' \
        -e 's/^(\s*)disable-cache(\s*=\s*)([Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn]|1|)(\s*)(#.*)?$/\1cache-type\2none\4\5/g' \
        -e 's/^(\s*disable-cache\s*=\s*)([Ff][Aa][Ll][Ss][Ee]|[Nn][Oo]|[Oo][Ff][Ff]|0)(\s*)(#.*)?$/#\1\2\3\4/g' \
        "$CONF"
fi

# Comment out some obsolete configuration parameters.
if grep -qE '^\s*(autoseed-updated|cache-index|enable-http-connect-requests|index-ipns-id|announce-in-bep5-swarm|cache-local-capacity|disable-cache|seed-content)\s*=' "$CONF" && ! has_help_arg "$@"; then
    sed -i -E 's/^(\s*)(autoseed-updated|cache-index|enable-http-connect-requests|index-ipns-id|announce-in-bep5-swarm|cache-local-capacity|disable-cache|seed-content)(\s*=.*)/##\1\2\3  # obsolete/' "$CONF"
fi

# Update TCP/TLS endpoint file name.
if ! has_help_arg "$@"; then
    if [ -e "$REPO/endpoint-tls" ]; then
        mv -n "$REPO/endpoint-tls" "$REPO/endpoint-tcp-tls"
    fi
fi

# Update BEP44 key file names.
if ! has_help_arg "$@"; then
    if [ -e "$REPO/bt-private-key" ]; then
        mv -n "$REPO/bt-private-key" "$REPO/ed25519-private-key"
    fi
    if [ -e "$REPO/bt-public-key" ]; then
        mv -n "$REPO/bt-public-key" "$REPO/ed25519-public-key"
    fi
    if [ -e "$REPO/bep44-private-key" ]; then
        mv -n "$REPO/bep44-private-key" "$REPO/ed25519-private-key"
    fi
    if [ -e "$REPO/bep44-public-key" ]; then
        mv -n "$REPO/bep44-public-key" "$REPO/ed25519-public-key"
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
