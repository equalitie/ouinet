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
asio-ipfs;eQualit.ie, Inc.;${SRC}/modules/asio-ipfs/LICENSE
boost;various;/usr/local/src/boost_1_67_0/LICENSE_1_0.txt
golang;${BUILD}/golang/AUTHORS;${BUILD}/golang/LICENSE
go-ipfs;various;${BUILD}/modules/asio-ipfs/go-ipfs-prefix/src/go-ipfs/LICENSE
i2pd;The PurpleI2P Project;${SRC}/src/ouiservice/i2p/i2pd/LICENSE
gpg-error;${BUILD}/gpg_error/src/gpg_error/AUTHORS;${BUILD}/gpg_error/src/gpg_error/COPYING.LIB
gcrypt;${BUILD}/gcrypt/src/gcrypt/AUTHORS;${BUILD}/gcrypt/src/gcrypt/COPYING.LIB
json;Niels Lohmann;${BUILD}/json/src/json/LICENSE.MIT
uri;Glyn Matthews;${BUILD}/uri/src/uri/LICENSE_1_0.txt
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
