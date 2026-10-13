/* Native Windows staged raw image replacement. SPDX-License-Identifier: MIT */
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "image_update.h"
#include "win32_files.h"
/* 0 staging, 1 cancelled, 2 commit decision taken. Cancellation arriving after
   the commit decision cannot retroactively roll back an atomic replacement. */
static volatile LONG cancel_state;
void image_update_cancel(void) { InterlockedCompareExchange(&cancel_state, 1, 0); }
struct ImageUpdate {
    char *original, *stage;
    HANDLE source;
    BY_HANDLE_FILE_INFORMATION identity, stage_identity;
};
static BOOL same_file(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b)
{
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}
static int lock_exclusive(HANDLE h)
{
    OVERLAPPED ov = {0};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                    &ov))
        return kv_win32_error(GetLastError());
    return 0;
}
static int check_identity(const char *path, const BY_HANDLE_FILE_INFORMATION *expected)
{
    HANDLE h =
        kv_win32_open(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    BY_HANDLE_FILE_INFORMATION current;
    BOOL ok = GetFileInformationByHandle(h, &current);
    DWORD error = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kv_win32_error(error);
    if (!same_file(expected, &current)) {
        errno = EIO;
        return -1;
    }
    return 0;
}
static int clone_sparse(HANDLE source, HANDLE target, LONGLONG size)
{
    DWORD returned;
    BOOL sparse = DeviceIoControl(target, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
    LARGE_INTEGER pos;
    pos.QuadPart = size;
    if (!SetFilePointerEx(target, pos, NULL, FILE_BEGIN) || !SetEndOfFile(target))
        return kv_win32_error(GetLastError());
    pos.QuadPart = 0;
    if (!SetFilePointerEx(source, pos, NULL, FILE_BEGIN))
        return kv_win32_error(GetLastError());
    unsigned char buffer[65536];
    LONGLONG offset = 0;
    while (offset < size) {
        if (InterlockedCompareExchange(&cancel_state, 0, 0) == 1) {
            errno = EINTR;
            return -1;
        }
        DWORD count = size - offset < (LONGLONG)sizeof(buffer) ? (DWORD)(size - offset)
                                                               : sizeof(buffer),
              read;
        if (!ReadFile(source, buffer, count, &read, NULL))
            return kv_win32_error(GetLastError());
        if (read != count) {
            errno = EIO;
            return -1;
        }
        BOOL nonzero = FALSE;
        for (DWORD i = 0; i < read; i++)
            if (buffer[i]) {
                nonzero = TRUE;
                break;
            }
        if (!sparse || nonzero) {
            pos.QuadPart = offset;
            if (!SetFilePointerEx(target, pos, NULL, FILE_BEGIN))
                return kv_win32_error(GetLastError());
            DWORD done = 0;
            while (done < read) {
                DWORD written;
                if (!WriteFile(target, buffer + done, read - done, &written, NULL))
                    return kv_win32_error(GetLastError());
                if (!written) {
                    errno = EIO;
                    return -1;
                }
                done += written;
            }
        }
        offset += read;
    }
    if (!FlushFileBuffers(target))
        return kv_win32_error(GetLastError());
    return 0;
}
ImageUpdate *image_update_begin(const char *path)
{
    InterlockedExchange(&cancel_state, 0);
    ImageUpdate *u = calloc(1, sizeof(*u));
    if (!u) {
        errno = ENOMEM;
        return NULL;
    }
    u->source = INVALID_HANDLE_VALUE;
    u->original = kv_win32_realpath(path);
    if (!u->original)
        goto fail;
    u->source =
        kv_win32_open(u->original, GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    if (u->source == INVALID_HANDLE_VALUE || lock_exclusive(u->source))
        goto fail;
    LARGE_INTEGER size;
    if (!GetFileInformationByHandle(u->source, &u->identity) || !GetFileSizeEx(u->source, &size)) {
        kv_win32_error(GetLastError());
        goto fail;
    }
    if (size.QuadPart <= 0 || size.QuadPart % 512) {
        errno = EINVAL;
        goto fail;
    }
    if (check_identity(u->original, &u->identity))
        goto fail;
    size_t capacity = strlen(u->original) + 80;
    u->stage = malloc(capacity);
    if (!u->stage) {
        errno = ENOMEM;
        goto fail;
    }
    HANDLE target = INVALID_HANDLE_VALUE;
    for (unsigned i = 0; i < 128; i++) {
        snprintf(u->stage, capacity, "%s.update-%lu-%llu-%u", u->original,
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(), i);
        target =
            kv_win32_open(u->stage, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, 0);
        if (target != INVALID_HANDLE_VALUE)
            break;
        if (errno != EEXIST) {
            free(u->stage);
            u->stage = NULL;
            goto fail;
        }
    }
    if (target == INVALID_HANDLE_VALUE) {
        free(u->stage);
        u->stage = NULL;
        errno = EEXIST;
        goto fail;
    }
    int result = clone_sparse(u->source, target, size.QuadPart);
    if (!result && !GetFileInformationByHandle(target, &u->stage_identity))
        result = kv_win32_error(GetLastError());
    int saved = errno;
    CloseHandle(target);
    errno = saved;
    if (result)
        goto fail;
    if (InterlockedCompareExchange(&cancel_state, 0, 0) == 1) {
        errno = EINTR;
        goto fail;
    }
    return u;
fail : {
    int saved = errno;
    image_update_finish(u, FALSE);
    errno = saved;
    return NULL;
}
}
const char *image_update_path(ImageUpdate *u) { return u->stage; }
int image_update_finish(ImageUpdate *u, BOOL success)
{
    if (!u)
        return 0;
    int result = 0;
    HANDLE target = INVALID_HANDLE_VALUE;
    if (success) {
        target =
            kv_win32_open(u->stage, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
        if (target == INVALID_HANDLE_VALUE || lock_exclusive(target) ||
            check_identity(u->stage, &u->stage_identity) ||
            check_identity(u->original, &u->identity))
            result = -1;
        if (!result && !FlushFileBuffers(target))
            result = kv_win32_error(GetLastError());
        wchar_t *original = NULL, *stage = NULL;
        if (!result) {
            original = kv_win32_utf16(u->original);
            stage = kv_win32_utf16(u->stage);
            if (!original || !stage)
                result = -1;
        }
        /* ReplaceFileW opens the replacement without file sharing. Its data
           handle must close first; the original lock stays held throughout. */
        if (target != INVALID_HANDLE_VALUE) {
            CloseHandle(target);
            target = INVALID_HANDLE_VALUE;
        }
        if (!result && InterlockedCompareExchange(&cancel_state, 2, 0) != 0) {
            errno = EINTR;
            result = -1;
        }
        /* The source lock remains held through replacement, so another VM
           cannot access the original while commit is pending. */
        if (!result && !ReplaceFileW(original, stage, NULL, 0, NULL, NULL))
            result = kv_win32_error(GetLastError());
        free(original);
        free(stage);
    }
    int saved = errno;
    if (target != INVALID_HANDLE_VALUE)
        CloseHandle(target);
    if (u->stage) {
        wchar_t *stage = kv_win32_utf16(u->stage);
        if (stage) {
            DeleteFileW(stage);
            free(stage);
        }
    }
    if (u->source != INVALID_HANDLE_VALUE)
        CloseHandle(u->source);
    free(u->original);
    free(u->stage);
    free(u);
    errno = saved;
    return result;
}
