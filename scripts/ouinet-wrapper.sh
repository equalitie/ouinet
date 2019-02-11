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
INJECTOR_LOOP_ADDR=127.7.2.1
INJECTOR_TCP_PORT=7070
INJECTOR_TLS_PORT=7077

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
        sed -i -E "s/^#?(listen-on-tcp\s*=\s*)127.0.0.1:[0-9]+(.*)/\1${INJECTOR_LOOP_ADDR}:${INJECTOR_TCP_PORT}\2/" "$CONF"
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
fi

# Configure the I2P daemon at the injector.
if [ "$PROG" = injector ]; then
    if grep -q '^\s*listen-on-i2p\s*=\s*true\b' "$CONF"; then
        sed -i -E 's/^(\s*listen-on-i2p\s*=\s*true\b.*)/##\1/' "$CONF"  # disable legacy I2P support
    fi

    # Convert legacy key so that it can be used by the daemon.
    INJECTOR_I2P_LEGACY_KEY="$REPO/i2p/i2p-private-key"  # Base64-encoded
    INJECTOR_I2P_KEY=/var/lib/i2pd/ouinet-injector-keys.dat  # raw
    INJECTOR_I2P_BACKUP_KEY="$REPO/i2p-private-key"  # a copy of the above
    if [ -e "$INJECTOR_I2P_LEGACY_KEY" -a ! -e "$INJECTOR_I2P_BACKUP_KEY" ]; then
        # The daemon uses non-standard Base64, see `T64` in `i2pd/libi2pd/Base.cpp`.
        tr -- -~ +/ < "$INJECTOR_I2P_LEGACY_KEY" | base64 -d > "$INJECTOR_I2P_BACKUP_KEY"
    fi
    # Always use backed-up I2P key (for container upgrades).
    touch "$INJECTOR_I2P_KEY"
    chown i2pd:i2pd "$INJECTOR_I2P_KEY"
    chmod 0640 "$INJECTOR_I2P_KEY"
    cat "$INJECTOR_I2P_BACKUP_KEY" > "$INJECTOR_I2P_KEY"

    if ! grep -q '^\s*ipv6\s*=\s*true\b' /etc/i2pd/i2pd.conf; then
        sed -i -E 's/^#*\s*(ipv6\s*=\s*)false(\b.*)/\1true\2/' /etc/i2pd/i2pd.conf  # enable IPv6
    fi

    if grep -q '^[^#]' /etc/i2pd/tunnels.conf; then
        sed -i -E 's/^([^#].*)/#\1/' /etc/i2pd/tunnels.conf  # disable default tunnels
    fi

    if [ ! -e /etc/i2pd/tunnels.conf.d/ouinet-injector.conf ]; then
        cat > /etc/i2pd/tunnels.conf.d/ouinet-injector.conf <<- EOF
		[ouinet-injector]
		type=server
		host=$INJECTOR_LOOP_ADDR
		port=$INJECTOR_TCP_PORT
		keys=$(basename "$INJECTOR_I2P_KEY")
		signaturetype=7
		inbound.quantity=3
		outbound.quantity=3
		inbound.length=1
		outbound.length=1
		i2p.streaming.initialAckDelay=20
		EOF
    fi

    # Start the I2P daemon.
    # Cron is used to allow rotation of logs via logrotate.
    /etc/init.d/cron start
    /etc/init.d/i2pd start

    # Attempt to show injector I2P endpoint.
    i2p_tuns_url='http://127.0.0.1:7070/?page=i2p_tunnels'
    i2p_dests_pfx='http://127.0.0.1:7070/?page=local_destination&b32='
    for _ in $(seq 10); do
        i2p_b32_ep=$(wget -qO- "$i2p_tuns_url" | sed -nE 's/.*\bouinet-injector\b.*\b(\S+.b32.i2p):[0-9]+.*/\1/p')
        if [ "$i2p_b32_ep" ]; then
            echo "$i2p_b32_ep" > "$REPO/endpoint-i2p"
            echo "Injector I2P endpoint (Base32): $i2p_b32_ep"
            i2p_b64_ep=$(wget -qO- "$i2p_dests_pfx$i2p_b32_ep" | sed -nE 's#.*<textarea[^>]*>([^<]+)</textarea>.*#\1#p')
            echo "Injector I2P endpoint (Base64): $i2p_b64_ep"
            if [ ! -e "$INJECTOR_I2P_BACKUP_KEY" ]; then
                cat "$INJECTOR_I2P_KEY" > "$INJECTOR_I2P_BACKUP_KEY"  # ensure that it survives upgrades
            fi
            break
        fi
        sleep 1
    done
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
