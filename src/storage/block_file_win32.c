/* Native Windows raw disks with sparse memory overlays. SPDX-License-Identifier: MIT */
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "block_file.h"
#include "win32_files.h"
#define SECTOR_BYTES 512
#define OVERLAY_BUCKETS 1024
typedef struct Sector {
    struct Sector *next;
    uint64_t number;
    uint8_t data[SECTOR_BYTES];
} Sector;
typedef struct {
    HANDLE handle;
    uint64_t sectors;
    BlockDeviceModeEnum mode;
    Sector *overlay[OVERLAY_BUCKETS];
} FileDisk;
static int64_t sector_count(BlockDevice *bs) { return ((FileDisk *)bs->opaque)->sectors; }
static BOOL valid_request(FileDisk *d, uint64_t s, int n)
{
    return n >= 0 && s <= d->sectors && (uint64_t)n <= d->sectors - s;
}
static Sector **find_sector(FileDisk *d, uint64_t s)
{
    Sector **p = &d->overlay[s % OVERLAY_BUCKETS];
    while (*p && (*p)->number != s)
        p = &(*p)->next;
    return p;
}
static int transfer(HANDLE h, uint64_t offset, void *buffer, uint64_t length, BOOL writing)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN))
        return kv_win32_error(GetLastError());
    uint8_t *p = buffer;
    while (length) {
        DWORD chunk = length > 0x40000000 ? 0x40000000 : (DWORD)length, done;
        BOOL ok =
            writing ? WriteFile(h, p, chunk, &done, NULL) : ReadFile(h, p, chunk, &done, NULL);
        if (!ok)
            return kv_win32_error(GetLastError());
        if (!done) {
            errno = EIO;
            return -1;
        }
        p += done;
        length -= done;
    }
    return 0;
}
static int disk_read(BlockDevice *bs, uint64_t s, uint8_t *b, int n, BlockDeviceCompletionFunc *cb,
                     void *opaque)
{
    (void)cb;
    (void)opaque;
    FileDisk *d = bs->opaque;
    if (!valid_request(d, s, n)) {
        errno = EINVAL;
        return -1;
    }
    if (transfer(d->handle, s * SECTOR_BYTES, b, (uint64_t)n * SECTOR_BYTES, FALSE))
        return -1;
    if (d->mode == BF_MODE_SNAPSHOT) {
        for (int i = 0; i < n; i++) {
            Sector *p = *find_sector(d, s + i);
            if (p)
                memcpy(b + (size_t)i * SECTOR_BYTES, p->data, SECTOR_BYTES);
        }
    }
    return 0;
}
static int disk_write(BlockDevice *bs, uint64_t s, const uint8_t *b, int n,
                      BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb;
    (void)opaque;
    FileDisk *d = bs->opaque;
    if (d->mode == BF_MODE_RO || !valid_request(d, s, n)) {
        errno = EINVAL;
        return -1;
    }
    if (d->mode != BF_MODE_SNAPSHOT)
        return transfer(d->handle, s * SECTOR_BYTES, (void *)b, (uint64_t)n * SECTOR_BYTES, TRUE);
    for (int i = 0; i < n; i++) {
        Sector **p = find_sector(d, s + i);
        if (!*p) {
            *p = malloc(sizeof(**p));
            if (!*p) {
                errno = ENOMEM;
                return -1;
            }
            (*p)->number = s + i;
            (*p)->next = NULL;
        }
        memcpy((*p)->data, b + (size_t)i * SECTOR_BYTES, SECTOR_BYTES);
    }
    return 0;
}
void block_file_end(BlockDevice *bs)
{
    if (!bs)
        return;
    FileDisk *d = bs->opaque;
    CloseHandle(d->handle);
    for (size_t i = 0; i < OVERLAY_BUCKETS; i++) {
        Sector *p = d->overlay[i];
        while (p) {
            Sector *next = p->next;
            free(p);
            p = next;
        }
    }
    free(d);
    free(bs);
}
BlockDevice *block_file_init(const char *path, BlockDeviceModeEnum mode)
{
    if (mode != BF_MODE_RO && mode != BF_MODE_RW && mode != BF_MODE_SNAPSHOT) {
        errno = EINVAL;
        return NULL;
    }
    HANDLE h =
        kv_win32_open(path, GENERIC_READ | (mode == BF_MODE_RW ? GENERIC_WRITE : 0),
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    OVERLAPPED lock = {0};
    BY_HANDLE_FILE_INFORMATION identity, current;
    LARGE_INTEGER size;
    if (!LockFileEx(h,
                    LOCKFILE_FAIL_IMMEDIATELY | (mode == BF_MODE_RW ? LOCKFILE_EXCLUSIVE_LOCK : 0),
                    0, MAXDWORD, MAXDWORD, &lock) ||
        !GetFileInformationByHandle(h, &identity) || !GetFileSizeEx(h, &size)) {
        kv_win32_error(GetLastError());
        goto fail;
    }
    HANDLE check =
        kv_win32_open(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    if (check == INVALID_HANDLE_VALUE)
        goto fail;
    BOOL ok = GetFileInformationByHandle(check, &current);
    DWORD error = GetLastError();
    CloseHandle(check);
    if (!ok) {
        kv_win32_error(error);
        goto fail;
    }
    if (identity.dwVolumeSerialNumber != current.dwVolumeSerialNumber ||
        identity.nFileIndexHigh != current.nFileIndexHigh ||
        identity.nFileIndexLow != current.nFileIndexLow) {
        errno = EIO;
        goto fail;
    }
    if (size.QuadPart <= 0 || size.QuadPart % SECTOR_BYTES) {
        errno = EINVAL;
        goto fail;
    }
    FileDisk *d = calloc(1, sizeof(*d));
    BlockDevice *bs = calloc(1, sizeof(*bs));
    if (!d || !bs) {
        free(d);
        free(bs);
        errno = ENOMEM;
        goto fail;
    }
    d->handle = h;
    d->mode = mode;
    d->sectors = size.QuadPart / SECTOR_BYTES;
    bs->opaque = d;
    bs->get_sector_count = sector_count;
    bs->read_async = disk_read;
    bs->write_async = disk_write;
    return bs;
fail : {
    int saved = errno;
    CloseHandle(h);
    errno = saved;
    return NULL;
}
}
