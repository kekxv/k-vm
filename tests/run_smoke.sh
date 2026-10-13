#!/bin/sh
set -eu
export K_VM_BINARY="${TEST_SRCDIR}/${TEST_WORKSPACE}/k-vm"
exec python3 "${TEST_SRCDIR}/${TEST_WORKSPACE}/tests/smoke_test.py"
