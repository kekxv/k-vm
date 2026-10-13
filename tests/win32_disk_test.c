/* Native Windows raw disk and transaction contracts. SPDX-License-Identifier: MIT */
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "block_file.h"
#include "image_update.h"
static void fill(const char *path, unsigned char value)
{
    HANDLE h =
        CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    unsigned char b[1024];
    memset(b, value, sizeof(b));
    DWORD n;
    assert(WriteFile(h, b, sizeof(b), &n, NULL) && n == sizeof(b));
    CloseHandle(h);
}
static void check(const char *path, unsigned char value)
{
    BlockDevice *d = block_file_init(path, BF_MODE_RO);
    assert(d);
    unsigned char b[1024];
    assert(d->read_async(d, 0, b, 2, NULL, NULL) == 0);
    for (size_t i = 0; i < sizeof(b); i++)
        assert(b[i] == value);
    block_file_end(d);
}
int main(void)
{
    char dir[MAX_PATH], path[MAX_PATH], other[MAX_PATH];
    assert(GetTempPathA(sizeof(dir), dir));
    assert(GetTempFileNameA(dir, "kvd", 0, path));
    assert(GetTempFileNameA(dir, "kvd", 0, other));
    fill(path, 0x31);
    unsigned char b[1024];
    memset(b, 0x62, sizeof(b));
    BlockDevice *a = block_file_init(path, BF_MODE_RO), *r = block_file_init(path, BF_MODE_RO);
    assert(a && r);
    assert(!block_file_init(path, BF_MODE_RW));
    assert(!image_update_begin(path));
    assert(a->write_async(a, 0, b, 1, NULL, NULL) < 0);
    assert(a->read_async(a, 2, b, 1, NULL, NULL) < 0);
    block_file_end(a);
    block_file_end(r);
    a = block_file_init(path, BF_MODE_SNAPSHOT);
    assert(a && a->get_sector_count(a) == 2);
    assert(a->write_async(a, 0, b, 2, NULL, NULL) == 0);
    memset(b, 0, sizeof(b));
    assert(a->read_async(a, 0, b, 2, NULL, NULL) == 0);
    assert(b[0] == 0x62);
    block_file_end(a);
    check(path, 0x31);
    a = block_file_init(path, BF_MODE_RW);
    assert(a);
    assert(!block_file_init(path, BF_MODE_RO));
    memset(b, 0x42, sizeof(b));
    assert(a->write_async(a, 0, b, 2, NULL, NULL) == 0);
    block_file_end(a);
    check(path, 0x42);
    ImageUpdate *u = image_update_begin(path);
    assert(u);
    assert(!block_file_init(path, BF_MODE_RO));
    fill(image_update_path(u), 0x53);
    assert(image_update_finish(u, FALSE) == 0);
    check(path, 0x42);
    u = image_update_begin(path);
    assert(u);
    fill(image_update_path(u), 0x53);
    image_update_cancel();
    assert(image_update_finish(u, TRUE) < 0);
    check(path, 0x42);
    u = image_update_begin(path);
    assert(u);
    fill(image_update_path(u), 0x53);
    assert(image_update_finish(u, TRUE) == 0);
    check(path, 0x53);
    u = image_update_begin(path);
    assert(u);
    assert(DeleteFileA(image_update_path(u)));
    assert(image_update_finish(u, TRUE) < 0);
    check(path, 0x53);
    u = image_update_begin(path);
    assert(u);
    fill(other, 0x64);
    assert(ReplaceFileA(path, other, NULL, 0, NULL, NULL));
    assert(image_update_finish(u, TRUE) < 0);
    check(path, 0x64);
    fill(path, 0x11);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size;
    size.QuadPart = 513;
    assert(SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h));
    CloseHandle(h);
    assert(!block_file_init(path, BF_MODE_RO));
    assert(!image_update_begin(path));
    /* Sparse raw images must support disk I/O beyond the 32-bit offset limit. */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    DWORD returned;
    assert(DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL));
    size.QuadPart = ((LONGLONG)1 << 32) + 512;
    assert(SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h));
    CloseHandle(h);
    a = block_file_init(path, BF_MODE_RW);
    assert(a);
    memset(b, 0x75, 512);
    assert(a->write_async(a, ((uint64_t)1 << 32) / 512, b, 1, NULL, NULL) == 0);
    memset(b, 0, 512);
    assert(a->read_async(a, ((uint64_t)1 << 32) / 512, b, 1, NULL, NULL) == 0);
    assert(b[0] == 0x75);
    assert(a->read_async(a, UINT64_MAX, b, 1, NULL, NULL) < 0);
    assert(a->read_async(a, 0, b, -1, NULL, NULL) < 0);
    block_file_end(a);
    DeleteFileA(path);
    DeleteFileA(other);
    puts("win32 disk tests passed");
    return 0;
}
