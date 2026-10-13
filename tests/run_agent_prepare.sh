#!/bin/sh
set -eu
exec python3 "${TEST_SRCDIR}/${TEST_WORKSPACE}/tools/agent_image/test_prepare.py"
