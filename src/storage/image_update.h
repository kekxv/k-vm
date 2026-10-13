/* SPDX-License-Identifier: MIT */
#ifndef K_VM_IMAGE_UPDATE_H
#define K_VM_IMAGE_UPDATE_H
#include "cutils.h"
typedef struct ImageUpdate ImageUpdate;
/* Async-signal-safe cancellation request during staging. */
void image_update_cancel(void);
ImageUpdate *image_update_begin(const char *path);
const char *image_update_path(ImageUpdate *update);
/* Commit only on verified guest success. Always destroys transaction. */
int image_update_finish(ImageUpdate *update, BOOL success);
#endif
