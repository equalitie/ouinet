#!/bin/sh
# Creates a ``licenses`` directory in the current directory
# as per the subdir name, authors string (or file) and license string (or file)
# listed in `$LICENSE_DATA` below.

set -e

if [ $# -ne 2 ]; then
    echo "Usage: $(basename "$0") SOURCE_DIR BUILD_DIR" >&2
    exit 1
fi

SRC="$1"
BUILD="$2"

# DIR_NAME;AUTHORS;LICENSE
LICENSE_DATA="\
ouinet;eQualit.ie, Inc.;${SRC}/LICENSE
boost;various;${BUILD}/boost/src/built_boost/LICENSE_1_0.txt
i2pd;The PurpleI2P Project;${SRC}/src/ouiservice/i2p/i2pd/LICENSE
json;Niels Lohmann;${BUILD}/json/src/json/LICENSE.MIT
asio-utp;eQualit.ie, Inc;${SRC}/modules/asio-utp/LICENSE
cpp-upnp;eQualit.ie, Inc;${SRC}/modules/cpp-upnp/LICENSE
"

echo "$LICENSE_DATA" | (
    while read l; do
        test "$l" || continue

        dir="$(echo "$l" | cut -d\; -f1)"
        auth="$(echo "$l" | cut -d\; -f2)"
        lic="$(echo "$l" | cut -d\; -f3)"

        mkdir -p "licenses/$dir"
        if [ -e "$auth" ]; then
            cp "$auth" "licenses/$dir/authors.txt"
        else
            echo "$auth" > "licenses/$dir/authors.txt"
        fi
        if [ -e "$lic" ]; then
            cp "$lic" "licenses/$dir/license.txt"
        else
            echo "$lic" > "licenses/$dir/license.txt"
        fi
    done
)
