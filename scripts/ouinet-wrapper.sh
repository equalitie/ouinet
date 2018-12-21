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

CLIENT_PROXY_PORT=8077
INJECTOR_TCP_PORT=7070
INJECTOR_TLS_PORT=7077

if [ ! -d "$REPO" ] && ! has_help_arg "$@"; then
    cp -r "$INST/repo-templates/$PROG" "$REPO"

    # Set a well-known client HTTP proxy port.
    if [ "$PROG" = client ]; then
        sed -i -E "s/^(listen-on-tcp\s*=\s*.*:)[0-9]+(.*)/\1${CLIENT_PROXY_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector loopback TCP port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tcp\s*=\s*127.0.0.1:)[0-9]+(.*)/\1${INJECTOR_TCP_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector TLS port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tls\s*=\s*:::)[0-9]+(.*)/\1${INJECTOR_TLS_PORT}\2/" "$CONF"
    fi

    # Generate a random password for injector credentials.
    if [ "$PROG" = injector ]; then
        password=$(dd if=/dev/urandom bs=1024 count=1 status=none | md5sum | cut -f1 -d' ')
        sed -i -E "s/^(credentials\s*=\s*).*/\1ouinet:$password/" "$CONF"
    fi

    # Configure the I2P daemon.
    if [ "$PROG" = injector ]; then
        sed -i -E 's/^#*\s*(ipv6\s*=\s*)false(\b.*)/\1true\2/' /etc/i2pd/i2pd.conf  # enable IPv6
        sed -i -E 's/^([^#].*)/#\1/' /etc/i2pd/tunnels.conf  # disable default tunnels
        cat > /etc/i2pd/tunnels.conf.d/ouinet-injector.conf <<- EOF
		[ouinet-injector]
		type=server
		host=127.0.0.1
		port=$INJECTOR_TCP_PORT
		inport=$INJECTOR_TCP_PORT
		keys=ouinet-injector-keys.dat
		signaturetype=7
		inbound.quantity=3
		outbound.quantity=3
		inbound.length=1
		outbound.length=1
		i2p.streaming.initialAckDelay=20
		EOF
    fi

    # Enable and start the I2P daemon.
    if [ "$PROG" = injector ]; then
        systemctl enable i2pd
        systemctl start i2pd
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
