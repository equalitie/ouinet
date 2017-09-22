#!/bin/bash

set -e

HTTP_OK=200

function do_curl {
    local code=$(curl $1 -o /dev/null -w "%{http_code}" --silent --show-error "${@:3}")
    if [ $code != $HTTP_OK ]; then
        echo "Expected HTTP response $2 but received $code"
        return 1
    fi
    return 0
}

if [ ! -f "$1" -o ! -x "$1" ]; then
    echo "First argument must point to the ouinet executable"
    exit 1
fi

ouinet=$1

trap 'kill -SIGTERM $(jobs -pr) 2>/dev/null || true; exit' HUP INT TERM EXIT

# Start the proxy
./$ouinet 0.0.0.0 8080 &
p=$!

sleep 0.5

# Test non secure HTTP request through the proxy
http_proxy=localhost:8080 do_curl asyncwired.com

# Test secure HTTP request through the proxy
https_proxy=localhost:8080 do_curl https://equalit.ie

kill $p
wait $p || true
