#!/bin/sh
set -eu
exec python3 "$TEST_SRCDIR/$TEST_WORKSPACE/tests/wsl_launcher_test.py"
