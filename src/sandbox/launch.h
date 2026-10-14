/* SPDX-License-Identifier: MIT */
#ifndef K_VM_SANDBOX_LAUNCH_H
#define K_VM_SANDBOX_LAUNCH_H
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
#include "block_file.h"
/* Parse/preflight/load. Returns 2 when isolated initialization is needed. */
int sandbox_prepare(int argc, char **argv, VirtMachineParams *p,
                    BlockDeviceModeEnum *disk_mode, BOOL *allow_ctrlc);
#endif
