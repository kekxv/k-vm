/* SPDX-License-Identifier: MIT */
#ifndef K_VM_SANDBOX_LAUNCH_H
#define K_VM_SANDBOX_LAUNCH_H
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
#include "block_file.h"
/* Parse launch options, preflight local resources and load boot assets. */
int sandbox_prepare(int argc, char **argv, VirtMachineParams *p,
                    BlockDeviceModeEnum *disk_mode, BOOL *allow_ctrlc);
#endif
