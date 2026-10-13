/* SPDX-License-Identifier: MIT */
#ifndef K_VM_CONSOLE_WIN32_H
#define K_VM_CONSOLE_WIN32_H
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include "cutils.h"
#include "virtio.h"
#include "machine.h"
CharacterDevice *kv_console_init(BOOL allow_ctrlc);
void kv_console_poll(VirtMachine *m);
BOOL kv_console_stop_requested(void);
void kv_console_restore(void);
#endif
#endif
