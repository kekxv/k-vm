#!/bin/sh
set -eu
export K_VM_PREPARE="${TEST_SRCDIR}/${TEST_WORKSPACE}/tools/prepare_linux_image.py"
exec python3 "${TEST_SRCDIR}/${TEST_WORKSPACE}/tests/prepare_image_test.py"
