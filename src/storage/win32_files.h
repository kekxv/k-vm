/* Native Windows file helpers. SPDX-License-Identifier: MIT */
#ifndef KV_WIN32_FILES_H
#define KV_WIN32_FILES_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stddef.h>
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
char *kv_win32_getenv_utf8(const char *key);
FILE *kv_win32_fopen(const char *path, const char *mode);
wchar_t *kv_win32_utf16(const char *text);
char *kv_win32_utf8(const wchar_t *text);
char *kv_win32_realpath(const char *path);
int kv_win32_error(DWORD error);
HANDLE kv_win32_open(const char *path, DWORD access, DWORD share,
                     DWORD disposition, DWORD extra_flags);
int kv_win32_rename_handle(HANDLE source, HANDLE destination_directory,
                           const char *name);
int kv_win32_valid_name(const char *name);
typedef struct {
    HANDLE handle;
    HANDLE
        *parents; /* ancestor directories, pinned against rename until close */
    size_t count;
} KVWin32File;
int kv_win32_relative(HANDLE root, const char *path, DWORD access, DWORD share,
                      DWORD disposition, int directory, KVWin32File *out);
void kv_win32_file_close(KVWin32File *file);
#endif
