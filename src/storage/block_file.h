/* SPDX-License-Identifier: MIT */
#ifndef K_VM_BLOCK_FILE_H
#define K_VM_BLOCK_FILE_H
#include "cutils.h"
#include "virtio.h"
typedef enum { BF_MODE_RO, BF_MODE_RW, BF_MODE_SNAPSHOT } BlockDeviceModeEnum;
/* Regular raw disk images, sector aligned. Failures return NULL and set errno. */
BlockDevice *block_file_init(const char *path, BlockDeviceModeEnum mode);
void block_file_end(BlockDevice *disk);
#endif
