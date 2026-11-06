#!/bin/bash

find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.cc' \) \
    -not -path './sdk/*' \
    -print0 | xargs -0 clang-format-19 --dry-run --Werror
