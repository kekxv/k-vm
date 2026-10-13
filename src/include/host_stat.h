/* Host stat layout and filesystem headers. SPDX-License-Identifier: MIT */
#ifndef K_VM_HOST_STAT_H
#define K_VM_HOST_STAT_H
#ifdef __APPLE__
#include <sys/mount.h>
#define HOST_STAT_ATIME(st) ((st).st_atimespec)
#define HOST_STAT_MTIME(st) ((st).st_mtimespec)
#define HOST_STAT_CTIME(st) ((st).st_ctimespec)
#else
#include <sys/statfs.h>
#define HOST_STAT_ATIME(st) ((st).st_atim)
#define HOST_STAT_MTIME(st) ((st).st_mtim)
#define HOST_STAT_CTIME(st) ((st).st_ctim)
#endif
#endif
