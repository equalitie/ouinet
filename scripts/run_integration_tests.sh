#!/bin/bash
cd ..

OUINET_BUILD_DIR=../scripts/ouinet-local-build/ PYTHONPATH=test/integration_test/ python -m twisted.trial test_http
