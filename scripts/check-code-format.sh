#!/bin/bash

find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.cc' \) \
    -not -path './build/*' \
    -print0 | xargs -0 clang-format --dry-run --Werror
