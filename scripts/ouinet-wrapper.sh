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
        sed -i -E "s/^#?(listen-on-tcp\s*=\s*)127.0.0.1:[0-9]+(.*)/\1${INJECTOR_LOOP_ADDR}:${INJECTOR_TCP_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector TCP/TLS port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-tcp-tls\s*=\s*:::)[0-9]+(.*)/\1${INJECTOR_TCP_TLS_PORT}\2/" "$CONF"
    fi

    # Set a well-known injector UTP/TLS port (and enable it).
    if [ "$PROG" = injector ]; then
        sed -i -E "s/^#?(listen-on-utp-tls\s*=\s*:::)[0-9]+(.*)/\1${INJECTOR_UTP_TLS_PORT}\2/" "$CONF"
    fi

    # Generate a random password for injector credentials.
    if [ "$PROG" = injector ]; then
        password=$(dd if=/dev/urandom bs=1024 count=1 status=none | md5sum | cut -f1 -d' ')
        sed -i -E "s/^(credentials\s*=\s*).*/\1ouinet:$password/" "$CONF"
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

# Convert completely changed configuration parameters.
if grep -qE '^\s*disable-cache\s*=\b' "$CONF" && ! has_help_arg "$@"; then
    # No Perl regular expressions for sed, bad luck.
    sed -i -E \
        -e 's/^(\s*)disable-cache(\s*=\s*)([Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn]|1|)(\s*)(#.*)?$/\1cache-type\2none\4\5/g' \
        -e 's/^(\s*disable-cache\s*=\s*)([Ff][Aa][Ll][Ss][Ee]|[Nn][Oo]|[Oo][Ff][Ff]|0)(\s*)(#.*)?$/#\1\2\3\4/g' \
        "$CONF"
fi

# Comment out some obsolete configuration parameters.
if grep -qE '^\s*(cache-index|index-ipns-id)\s*=' "$CONF" && ! has_help_arg "$@"; then
    sed -i -E 's/^(\s*)(cache-index|index-ipns-id)(\s*=.*)/##\1\2\3  # obsolete/' "$CONF"
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
        mv -n "$REPO/bt-public-key" "$REPO/bep44-public-key"
    fi
    if [ -e "$REPO/bep44-private-key" ]; then
        mv -n "$REPO/bep44-private-key" "$REPO/ed25519-private-key"
    fi
fi

# Configure the I2P daemon at the injector.
if [ "$PROG" = injector ] && ! has_help_arg "$@"; then
    if grep -q '^\s*listen-on-i2p\s*=\s*true\b' "$CONF"; then
        sed -i -E 's/^(\s*listen-on-i2p\s*=\s*true\b.*)/##\1/' "$CONF"  # disable legacy I2P support
    fi

    # Convert legacy key so that it can be used by the daemon.
    INJECTOR_I2P_LEGACY_KEY="$REPO/i2p/i2p-private-key"  # Base64-encoded
    INJECTOR_I2P_DAEMON_KEY=/var/lib/i2pd/ouinet-injector-keys.dat  # raw
    INJECTOR_I2P_BACKUP_KEY="$REPO/i2p-private-key"  # a copy of the above
    if [ -e "$INJECTOR_I2P_LEGACY_KEY" -a ! -e "$INJECTOR_I2P_BACKUP_KEY" ]; then
        # The daemon uses non-standard Base64, see `T64` in `i2pd/libi2pd/Base.cpp`.
        tr -- -~ +/ < "$INJECTOR_I2P_LEGACY_KEY" | base64 -d > "$INJECTOR_I2P_BACKUP_KEY"
    fi
    if [ -e "$INJECTOR_I2P_BACKUP_KEY" ]; then
        # Always use backed-up I2P key (for container upgrades).
        touch "$INJECTOR_I2P_DAEMON_KEY"
        chown i2pd:i2pd "$INJECTOR_I2P_DAEMON_KEY"
        chmod 0640 "$INJECTOR_I2P_DAEMON_KEY"
        cat "$INJECTOR_I2P_BACKUP_KEY" > "$INJECTOR_I2P_DAEMON_KEY"
    fi

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
		keys=$(basename "$INJECTOR_I2P_DAEMON_KEY")
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
                cat "$INJECTOR_I2P_DAEMON_KEY" > "$INJECTOR_I2P_BACKUP_KEY"  # ensure that it survives upgrades
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

# Prepend the installation path to use supporting binaries in there.
export PATH="$INST:$PATH"
if [ "$repo_arg" ]; then
    run "$@"
else
    run --repo "$REPO" "$@"
fi
