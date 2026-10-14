/* SPDX-License-Identifier: MIT */
#ifndef K_VM_SANDBOX_UPDATE_H
#define K_VM_SANDBOX_UPDATE_H
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
/* Parent waits/commits and returns exit status; child returns -1 to boot VM. */
int sandbox_update_run(VirtMachineParams *p, const char *operation);
#ifdef _WIN32
/* Call after virt_machine_end has closed all staged disk handles. */
int sandbox_update_complete(int vm_status);
#endif
#endif
