#!/bin/bash

PROFILE=$(mktemp -d)
http_proxy="http://localhost:8077/" firefox --no-remote --profile "${PROFILE}"
rm -rf ${PROFILE}
