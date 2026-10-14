/* Disk identity hints and host-download fallback. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "initialize.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
int main(void)
{
    char root[] = "/tmp/k-vm-initialize-XXXXXX", disk[512], stamp[512];
    CHECK(mkdtemp(root));
    snprintf(disk, sizeof(disk), "%s/disk", root);
    snprintf(stamp, sizeof(stamp), "%s/disk.pi-ready", root);
    FILE *f = fopen(disk, "wb"); CHECK(f); fputs("disk", f); fclose(f);
    CHECK(!sandbox_pi_initialized(disk));
    CHECK(sandbox_pi_record_initialized(disk) == 0);
    CHECK(sandbox_pi_initialized(disk));
    f = fopen(disk, "ab"); CHECK(f); fputs("changed", f); fclose(f);
    CHECK(!sandbox_pi_initialized(disk));
    CHECK(unlink(stamp) == 0);
    CHECK(symlink(disk, stamp) == 0);
    CHECK(sandbox_pi_record_initialized(disk) != 0);
    CHECK(sandbox_prefetch_node("bad;version", root) != 0);
    CHECK(setenv("PATH", "/nonexistent", 1) == 0);
    CHECK(sandbox_prefetch_node("24.21.0", root) == 127);
    CHECK(unlink(stamp) == 0); CHECK(unlink(disk) == 0); CHECK(rmdir(root) == 0);
    puts("Readiness follows disk identity; unsafe hints and absent host curl handled");
    return 0;
}
