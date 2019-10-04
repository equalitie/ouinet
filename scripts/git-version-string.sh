#!/bin/sh
ID=`git rev-parse HEAD 2>/dev/null || echo unknown-id`
BRANCH=`git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown-branch`
echo "$BRANCH $ID"
