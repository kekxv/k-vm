/* SPDX-License-Identifier: MIT */
#ifndef K_VM_WORKSPACE_H
#define K_VM_WORKSPACE_H
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
/* Export cwd and a readonly private bundle of selected configuration and startup. */
int workspace_prepare(VirtMachineParams *p, const char *agent, const char *config_dir,
                      int argc, const char *const *argv);
void workspace_cleanup(void);
int workspace_prepare_update(VirtMachineParams *p, const char *version);
BOOL workspace_update_succeeded(void);
#endif
