#!/bin/bash

find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.cc' \) \
    -print0 | xargs -0 clang-format-19 --dry-run --Werror --ferror-limit=5
