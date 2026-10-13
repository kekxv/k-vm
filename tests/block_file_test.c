/* Real raw image IO: transient writes must not alter the original disk. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "cutils.h"
#include "virtio.h"
#include "block_file.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
int main(void)
{
    char path[] = "/tmp/k-vm-block-XXXXXX";
    int fd = mkstemp(path);
    uint8_t a[512], b[512], out[1024];
    memset(a, 'a', sizeof(a)); memset(b, 'b', sizeof(b));
    CHECK(fd >= 0 && write(fd, a, 512) == 512 && write(fd, a, 512) == 512);
    BlockDevice *disk = block_file_init(path, BF_MODE_SNAPSHOT);
    CHECK(disk && disk->get_sector_count(disk) == 2);
    CHECK(disk->write_async(disk, 1, b, 1, NULL, NULL) == 0);
    CHECK(disk->read_async(disk, 0, out, 2, NULL, NULL) == 0);
    CHECK(!memcmp(out, a, 512) && !memcmp(out + 512, b, 512));
    CHECK(pread(fd, out, 512, 512) == 512 && !memcmp(out, a, 512));
    CHECK(disk->read_async(disk, 2, out, 1, NULL, NULL) < 0);
    CHECK(disk->write_async(disk, UINT64_MAX, b, 1, NULL, NULL) < 0);
    CHECK(disk->read_async(disk, 0, out, -1, NULL, NULL) < 0);
    block_file_end(disk);
    disk = block_file_init(path, BF_MODE_RO);
    CHECK(disk && disk->write_async(disk, 0, b, 1, NULL, NULL) < 0);
    block_file_end(disk);
    disk = block_file_init(path, BF_MODE_RW);
    CHECK(disk && disk->write_async(disk, 0, b, 1, NULL, NULL) == 0);
    CHECK(pread(fd, out, 512, 0) == 512 && !memcmp(out, b, 512));
    block_file_end(disk);
    /* 1 TB sparse file: snapshot allocation must not scale with disk size. */
    CHECK(ftruncate(fd, (off_t)1 << 40) == 0);
    disk = block_file_init(path, BF_MODE_SNAPSHOT);
    CHECK(disk && disk->get_sector_count(disk) == ((int64_t)1 << 31));
    CHECK(disk->write_async(disk, ((uint64_t)1 << 31) - 1, b, 1, NULL, NULL) == 0);
    CHECK(disk->read_async(disk, ((uint64_t)1 << 31) - 1, out, 1, NULL, NULL) == 0);
    CHECK(!memcmp(out, b, 512));
    block_file_end(disk);
    CHECK(ftruncate(fd, 513) == 0);
    CHECK(block_file_init(path, BF_MODE_RO) == NULL);
    close(fd); unlink(path);
    puts("Raw disk snapshots preserve base images and handle large sparse disks");
    return 0;
}
