/* Descriptor-relative host IO for sandbox exports. SPDX-License-Identifier: MIT */
#ifndef K_VM_HOST_IO_H
#define K_VM_HOST_IO_H
#include <sys/stat.h>
#include <stddef.h>
/* Paths are relative to a pinned directory. No symlink traversal is allowed. */
int host_open_directory(int root, const char *path);
int host_open_file(int root, const char *path, int flags);
int host_lstat(int root, const char *path, struct stat *st);
int host_readlink(int root, const char *path, char *buf, size_t size);
#endif
