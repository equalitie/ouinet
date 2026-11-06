#!/bin/bash

find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.cc' \) \
    -not -path './sdk/*' \
    -not -path './modules/*' \
    -not -path './ios/toolchain/*' \
    -not -path './src/ouiservice/i2p/i2pd/*' \
    -print0 | xargs -0 clang-format-19 --dry-run --Werror --verbose
