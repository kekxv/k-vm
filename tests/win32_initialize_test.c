/* Readiness hints must never truncate a hard-linked host file. SPDX-License-Identifier: MIT */
#include "win32_files.h"
#include "initialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s (error %lu)\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
int main(void)
{
    wchar_t temp[MAX_PATH], root[MAX_PATH], disk[MAX_PATH], stamp[MAX_PATH], victim[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"kvi", 0, root));
    CHECK(DeleteFileW(root));
    CHECK(CreateDirectoryW(root, NULL));
    swprintf(disk, MAX_PATH, L"%ls\\disk", root);
    swprintf(stamp, MAX_PATH, L"%ls\\disk.pi-ready", root);
    swprintf(victim, MAX_PATH, L"%ls\\other-host-file", root);
    char *d = kv_win32_utf8(disk), *v = kv_win32_utf8(victim);
    CHECK(d && v);
    FILE *f = kv_win32_fopen(d, "wb"); CHECK(f); fputs("disk", f); fclose(f);
    f = kv_win32_fopen(v, "wb"); CHECK(f); fputs("preserve me", f); fclose(f);
    CHECK(CreateHardLinkW(stamp, victim, NULL));
    CHECK(sandbox_pi_record_initialized(d) != 0);
    char content[32] = {0};
    f = kv_win32_fopen(v, "rb"); CHECK(f); CHECK(fread(content, 1, sizeof(content), f) == 11); fclose(f);
    CHECK(!strcmp(content, "preserve me"));
    CHECK(DeleteFileW(stamp));
    CHECK(!sandbox_pi_initialized(d));
    CHECK(sandbox_pi_record_initialized(d) == 0);
    CHECK(sandbox_pi_initialized(d));
    f = kv_win32_fopen(d, "ab"); CHECK(f); fputs("changed", f); fclose(f);
    CHECK(!sandbox_pi_initialized(d));
    CHECK(DeleteFileW(stamp)); CHECK(DeleteFileW(victim)); CHECK(DeleteFileW(disk));
    CHECK(RemoveDirectoryW(root)); free(d); free(v);
    puts("Win32 readiness detects disk changes and preserves hard-linked host files");
    return 0;
}
