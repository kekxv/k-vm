/* SPDX-License-Identifier: MIT */
#ifndef K_VM_INITIALIZE_H
#define K_VM_INITIALIZE_H
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
BOOL sandbox_pi_initialized(const char *disk);
int sandbox_pi_record_initialized(const char *disk);
int sandbox_initialize_image(const char *executable, const VirtMachineParams *p);
/* Returns 127 when host curl is unavailable (guest HTTPS fallback). */
int sandbox_prefetch_node(const char *version, const char *destination);
#endif
