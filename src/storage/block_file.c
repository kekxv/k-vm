/* Local raw disks with sparse, in-memory copy-on-write overlays. SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "block_file.h"

#define SECTOR_BYTES 512
#define OVERLAY_BUCKETS 1024

typedef struct Sector {
    struct Sector *next;
    uint64_t number;
    uint8_t data[SECTOR_BYTES];
} Sector;

typedef struct {
    int fd;
    uint64_t sectors;
    BlockDeviceModeEnum mode;
    Sector *overlay[OVERLAY_BUCKETS];
} FileDisk;

static int64_t sector_count(BlockDevice *disk)
{
    return ((FileDisk *)disk->opaque)->sectors;
}

static BOOL valid_request(FileDisk *disk, uint64_t sector, int n)
{
    return n >= 0 && sector <= disk->sectors && (uint64_t)n <= disk->sectors - sector;
}

static Sector **find_sector(FileDisk *disk, uint64_t number)
{
    Sector **p = &disk->overlay[number % OVERLAY_BUCKETS];
    while (*p && (*p)->number != number) p = &(*p)->next;
    return p;
}

static int disk_read(BlockDevice *bs, uint64_t sector, uint8_t *buf, int n,
                     BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    FileDisk *disk = bs->opaque;
    if (!valid_request(disk, sector, n)) return -1;
    size_t len = (size_t)n * SECTOR_BYTES, done = 0;
    while (done < len) {
        ssize_t ret = pread(disk->fd, buf + done, len - done, (off_t)sector * SECTOR_BYTES + done);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return -1;
        done += ret;
    }
    if (disk->mode == BF_MODE_SNAPSHOT) {
        for (int i = 0; i < n; ++i) {
            Sector *entry = *find_sector(disk, sector + i);
            if (entry) memcpy(buf + (size_t)i * SECTOR_BYTES, entry->data, SECTOR_BYTES);
        }
    }
    return 0;
}

static int disk_write(BlockDevice *bs, uint64_t sector, const uint8_t *buf, int n,
                      BlockDeviceCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    FileDisk *disk = bs->opaque;
    if (disk->mode == BF_MODE_RO || !valid_request(disk, sector, n)) return -1;
    if (disk->mode == BF_MODE_SNAPSHOT) {
        for (int i = 0; i < n; ++i) {
            Sector **entry = find_sector(disk, sector + i);
            if (!*entry) {
                *entry = malloc(sizeof(**entry));
                if (!*entry) return -1;
                (*entry)->next = NULL;
                (*entry)->number = sector + i;
            }
            memcpy((*entry)->data, buf + (size_t)i * SECTOR_BYTES, SECTOR_BYTES);
        }
    } else {
        size_t len = (size_t)n * SECTOR_BYTES, done = 0;
        while (done < len) {
            ssize_t ret = pwrite(disk->fd, buf + done, len - done, (off_t)sector * SECTOR_BYTES + done);
            if (ret < 0 && errno == EINTR) continue;
            if (ret <= 0) return -1;
            done += ret;
        }
    }
    return 0;
}

void block_file_end(BlockDevice *bs)
{
    if (!bs) return;
    FileDisk *disk = bs->opaque;
    close(disk->fd);
    for (size_t i = 0; i < OVERLAY_BUCKETS; ++i) {
        Sector *p = disk->overlay[i];
        while (p) { Sector *next = p->next; free(p); p = next; }
    }
    free(disk); free(bs);
}

BlockDevice *block_file_init(const char *path, BlockDeviceModeEnum mode)
{
    if (mode != BF_MODE_RO && mode != BF_MODE_RW && mode != BF_MODE_SNAPSHOT) {
        errno = EINVAL; return NULL;
    }
    int fd = open(path, (mode == BF_MODE_RW ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    if (flock(fd, (mode == BF_MODE_RW ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        int err = errno; close(fd); errno = err; return NULL;
    }
    struct stat st;
    if (fstat(fd, &st)) { int err = errno; close(fd); errno = err; return NULL; }
    struct stat current;
    if (stat(path, &current) || current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
        close(fd); errno = ESTALE; return NULL;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % SECTOR_BYTES) {
        close(fd); errno = EINVAL; return NULL;
    }
    FileDisk *disk = calloc(1, sizeof(*disk));
    BlockDevice *bs = calloc(1, sizeof(*bs));
    if (!disk || !bs) { close(fd); free(disk); free(bs); errno = ENOMEM; return NULL; }
    disk->fd = fd; disk->sectors = st.st_size / SECTOR_BYTES; disk->mode = mode;
    bs->opaque = disk; bs->get_sector_count = sector_count;
    bs->read_async = disk_read; bs->write_async = disk_write;
    return bs;
}
