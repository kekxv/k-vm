#!/bin/sh
set -eu
exec python3 "${TEST_SRCDIR}/${TEST_WORKSPACE}/tests/pi_install_test.py"
