/* Native Windows filesystem smoke and confinement regression tests. */
#include "cutils.h"
#include "fs.h"
#include "win32_files.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <winioctl.h>
/* Junction creation requires no symlink privilege on native Windows. */
static int junction(const wchar_t *path, const wchar_t *target) {
    struct {
        DWORD tag;
        WORD length, reserved;
        WORD sub_offset, sub_length, print_offset, print_length;
        wchar_t names[2048];
    } data;
    memset(&data, 0, sizeof(data));
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    wchar_t native[1024];
    if (wcslen(target) + 5 >= 1024)
        return 0;
    wcscpy(native, L"\\??\\");
    wcscat(native, target);
    size_t chars = wcslen(native);
    memcpy(data.names, native, (chars + 1) * sizeof(wchar_t));
    data.sub_length = chars * 2;
    data.print_offset = (chars + 1) * 2;
    size_t printed = wcslen(target);
    memcpy(data.names + chars + 1, target, (printed + 1) * 2);
    data.print_length = printed * 2;
    data.length = 8 + (chars + printed + 2) * 2;
    if (!CreateDirectoryW(path, NULL))
        return 0;
    HANDLE h = CreateFileW(
        path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(path);
        return 0;
    }
    DWORD n;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data,
                              data.length + 8, NULL, 0, &n, NULL);
    CloseHandle(h);
    if (!ok)
        RemoveDirectoryW(path);
    return ok;
}
int main(void) {
    assert(SetEnvironmentVariableW(L"KVM_TEST_UTF8", L"\u4e2d\u6587"));
    char *env = kv_win32_getenv_utf8("KVM_TEST_UTF8");
    assert(env && !strcmp(env, "\xe4\xb8\xad\xe6\x96\x87"));
    free(env);
    assert(SetEnvironmentVariableW(L"KVM_TEST_UTF8", NULL));
    assert(kv_win32_getenv_utf8("KVM_TEST_UTF8") == NULL);

    char tmp[MAX_PATH], root[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%skvm-fs-%lu-%lu", tmp,
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    assert(CreateDirectoryA(root, NULL));
    FSDevice *s = fs_confined_init(root, FALSE);
    assert(s);
    FSFile *r, *f;
    FSQID q[4];
    unsigned char buf[1024];
    FSStat st;
    assert(!s->fs_attach(s, &r, q, 0, "", ""));
    assert(!s->fs_walk(s, &f, q, r, 0, NULL));
    assert(!s->fs_create(s, q, f, "hello.txt", P9_O_RDWR | P9_O_EXCL, 0600, 0));
    assert(s->fs_write(s, f, 0, (const uint8_t *)"hello", 5) == 5);
    assert(s->fs_read(s, f, 0, buf, 5) == 5 && !memcmp(buf, "hello", 5));
    assert(!s->fs_stat(s, f, &st) && st.st_size == 5);
    assert(!s->fs_setattr(s, f, P9_SETATTR_SIZE, 0, 0, 0, 2, 0, 0, 0, 0));
    assert(!s->fs_stat(s, f, &st) && st.st_size == 2);
    s->fs_delete(s, f);
    char *legacy_name[] = {"hello.txt"};
    assert(s->fs_walk(s, &f, q, r, 1, legacy_name) == 1);
    /* Linux 4.15 leaves all unused setattr wire fields at UINT64_MAX. */
    assert(!s->fs_setattr(s, f, P9_SETATTR_MODE, 0600, 0, 0, UINT64_MAX,
                          UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX));
    s->fs_delete(s, f);
    /* Open fids preserve identity across guest rename and pathname replacement.
     */
    char *original[] = {"hello.txt"};
    assert(s->fs_walk(s, &f, q, r, 1, original) == 1);
    assert(!s->fs_stat(s, f, &st) && (st.st_mode & 0777) == 0600);
    assert(!s->fs_open(s, q, f, P9_O_RDWR, NULL, NULL));
    assert(!s->fs_renameat(s, r, "hello.txt", r, "open-renamed.txt"));
    assert(!s->fs_stat(s, f, &st) && st.st_size == 2);
    assert(!s->fs_setattr(s, f, P9_SETATTR_MODE | P9_SETATTR_SIZE, 0700, 0, 0,
                          1, 0, 0, 0, 0));
    assert(!s->fs_stat(s, f, &st) && st.st_size == 1 &&
           (st.st_mode & 0777) == 0700);
    FSFile *replacement;
    assert(!s->fs_walk(s, &replacement, q, r, 0, NULL));
    assert(!s->fs_create(s, q, replacement, "hello.txt", P9_O_RDWR | P9_O_EXCL,
                         0644, 0));
    assert(s->fs_write(s, replacement, 0, (const uint8_t *)"new", 3) == 3);
    assert(!s->fs_setattr(s, f, P9_SETATTR_SIZE, 0, 0, 0, 2, 0, 0, 0, 0));
    assert(!s->fs_stat(s, replacement, &st) && st.st_size == 3 &&
           (st.st_mode & 0777) == 0644);
    s->fs_delete(s, replacement);
    s->fs_delete(s, f);
    assert(!s->fs_unlinkat(s, r, "open-renamed.txt"));
    assert(!s->fs_walk(s, &f, q, r, 0, NULL));
    assert(!s->fs_create(s, q, f, "readonly", P9_O_RDWR | P9_O_EXCL, 0600, 0));
    assert(!s->fs_setattr(s, f, P9_SETATTR_MODE, 0400, 0, 0, 0, 0, 0, 0, 0));
    s->fs_delete(s, f);
    assert(!s->fs_renameat(s, r, "readonly", r, "readonly-renamed"));
    assert(!s->fs_unlinkat(s, r, "readonly-renamed"));
    assert(!s->fs_walk(s, &f, q, r, 0, NULL));
    assert(!s->fs_create(s, q, f, "readonly-target", P9_O_RDWR | P9_O_EXCL,
                         0600, 0));
    assert(!s->fs_setattr(s, f, P9_SETATTR_MODE, 0400, 0, 0, 0, 0, 0, 0, 0));
    s->fs_delete(s, f);
    assert(!s->fs_walk(s, &f, q, r, 0, NULL));
    assert(!s->fs_create(s, q, f, "replacement-source", P9_O_RDWR | P9_O_EXCL,
                         0600, 0));
    assert(s->fs_write(s, f, 0, (const uint8_t *)"replace", 7) == 7);
    s->fs_delete(s, f);
    char target_path[MAX_PATH];
    snprintf(target_path, sizeof(target_path), "%s\\readonly-target", root);
    char target_alias[MAX_PATH];
    snprintf(target_alias, sizeof(target_alias), "%s\\readonly-target-alias", root);
    assert(CreateHardLinkA(target_alias, target_path, NULL));
    HANDLE old_target = kv_win32_open(target_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    assert(old_target != INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO target_info;
    assert(GetFileInformationByHandleEx(old_target, FileBasicInfo, &target_info,
                                       sizeof(target_info)));
    assert(target_info.FileAttributes & FILE_ATTRIBUTE_READONLY);
    CloseHandle(old_target);
    assert(!s->fs_renameat(s, r, "replacement-source", r, "readonly-target"));
    old_target = kv_win32_open(target_alias, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    assert(old_target != INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO preserved_info;
    assert(GetFileInformationByHandleEx(old_target, FileBasicInfo, &preserved_info,
                                       sizeof(preserved_info)));
    assert(preserved_info.FileAttributes == target_info.FileAttributes);
    CloseHandle(old_target);
    assert(SetFileAttributesA(target_alias, FILE_ATTRIBUTE_NORMAL));
    assert(DeleteFileA(target_alias));
    char *replaced[] = {"readonly-target"};
    assert(s->fs_walk(s, &f, q, r, 1, replaced) == 1);
    assert(!s->fs_stat(s, f, &st) && st.st_size == 7 &&
           (st.st_mode & 0777) == 0600);
    s->fs_delete(s, f);
    assert(!s->fs_unlinkat(s, r, "readonly-target"));
    assert(!s->fs_mkdir(s, q, r, "readonly-nonempty", 0700, 0));
    char *nonempty[] = {"readonly-nonempty"};
    assert(s->fs_walk(s, &f, q, r, 1, nonempty) == 1);
    assert(!s->fs_walk(s, &replacement, q, f, 0, NULL));
    assert(!s->fs_create(s, q, replacement, "child", P9_O_RDWR, 0600, 0));
    s->fs_delete(s, replacement);
    assert(!s->fs_setattr(s, f, P9_SETATTR_MODE, 0400, 0, 0, 0, 0, 0, 0, 0));
    char nonempty_path[MAX_PATH];
    snprintf(nonempty_path, sizeof(nonempty_path), "%s\\readonly-nonempty",
             root);
    DWORD original_attrs = GetFileAttributesA(nonempty_path);
    assert(s->fs_unlinkat(s, r, "readonly-nonempty") == -P9_ENOTEMPTY);
    assert(GetFileAttributesA(nonempty_path) == original_attrs);
    assert(!s->fs_unlinkat(s, f, "child"));
    s->fs_delete(s, f);
    assert(!s->fs_unlinkat(s, r, "readonly-nonempty"));
    const char *bad[] = {"../escape", "x\\escape", "hello.txt:stream",
                         "NUL",       "con.txt",   "foo.",
                         "foo ",      "COM1",      "LPT9.log",
                         "CON .txt"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(!s->fs_walk(s, &f, q, r, 0, NULL));
        assert(s->fs_create(s, q, f, bad[i], P9_O_RDWR, 0600, 0) < 0);
        s->fs_delete(s, f);
    }
    assert(!s->fs_mkdir(s, q, r, "sub", 0700, 0));
    char *parts[] = {"sub", "nested.txt"};
    assert(s->fs_walk(s, &f, q, r, 1, parts) == 1);
    assert(!s->fs_create(s, q, f, "nested.txt", P9_O_RDWR, 0600, 0));
    /* An open descendant pins its containing directory against host moves. */
    char olddir[MAX_PATH], newdir[MAX_PATH];
    snprintf(olddir, sizeof(olddir), "%s\\sub", root);
    snprintf(newdir, sizeof(newdir), "%s\\moved", root);
    assert(!MoveFileA(olddir, newdir));
    s->fs_delete(s, f);
    assert(s->fs_walk(s, &f, q, r, 1, parts) == 1);
    assert(!s->fs_unlinkat(s, f, "nested.txt"));
    s->fs_delete(s, f);
    char linkpath[MAX_PATH];
    snprintf(linkpath, sizeof(linkpath), "%s\\escape-link", root);
    wchar_t *wl = kv_win32_utf16(linkpath), *wt = kv_win32_utf16(tmp);
    if (CreateSymbolicLinkW(wl, wt, SYMBOLIC_LINK_FLAG_DIRECTORY | 2) &&
        GetFileAttributesW(wl) != INVALID_FILE_ATTRIBUTES) {
        char *escape[] = {"escape-link"};
        assert(s->fs_walk(s, &f, q, r, 1, escape) == 0);
        s->fs_delete(s, f);
        assert(fs_confined_init(linkpath, FALSE) == NULL);
        assert(RemoveDirectoryA(linkpath));
    } else
        fprintf(stderr,
                "host does not permit directory symlink creation (%lu)\n",
                (unsigned long)GetLastError());
    if (junction(wl, wt)) {
        char *escape[] = {"escape-link"};
        assert(s->fs_walk(s, &f, q, r, 1, escape) == 0);
        s->fs_delete(s, f);
        assert(fs_confined_init(linkpath, FALSE) == NULL);
        char descendant[MAX_PATH];
        snprintf(descendant, sizeof(descendant), "%s\\escape-link\\escaped",
                 root);
        assert(kv_win32_open(descendant, GENERIC_WRITE, FILE_SHARE_READ,
                             CREATE_NEW, 0) == INVALID_HANDLE_VALUE);
        assert(RemoveDirectoryW(wl));
    } else
        fprintf(stderr, "host does not support junction creation\n");
    free(wl);
    free(wt);
    FSLock lock = {.type = P9_LOCK_TYPE_WRLCK, .start = 0, .length = 1};
    char *hello[] = {"hello.txt"};
    assert(s->fs_walk(s, &f, q, r, 1, hello) == 1);
    assert(!s->fs_open(s, q, f, P9_O_RDWR, NULL, NULL));
    assert(s->fs_lock(s, f, &lock) == P9_LOCK_SUCCESS);
    lock.type = P9_LOCK_TYPE_UNLCK;
    assert(s->fs_lock(s, f, &lock) == P9_LOCK_SUCCESS);
    s->fs_delete(s, f);

    assert(!s->fs_open(s, q, r, P9_O_RDONLY, NULL, NULL));
    assert(s->fs_readdir(s, r, 0, buf, sizeof(buf)) > 0);
    s->fs_close(s, r);
    int rename_result = s->fs_renameat(s, r, "hello.txt", r, "renamed.txt");
    assert(!rename_result);
    assert(!s->fs_unlinkat(s, r, "renamed.txt"));
    assert(!s->fs_unlinkat(s, r, "sub"));
    s->fs_delete(s, r);
    s->fs_end(s);
    free(s);
    s = fs_confined_init(root, TRUE);
    assert(s);
    assert(!s->fs_attach(s, &r, q, 0, "", ""));
    assert(s->fs_create(s, q, r, "denied", P9_O_RDWR, 0600, 0) == -P9_EPERM);
    s->fs_delete(s, r);
    s->fs_end(s);
    free(s);
    char *canonical_root = kv_win32_realpath(root);
    assert(canonical_root);
    size_t joined_size = strlen(canonical_root) + 80;
    char *joined = malloc(joined_size);
    assert(joined);
    snprintf(joined, joined_size, "%s/\xe4\xb8\xad\xe6\x96\x87.cfg",
             canonical_root);
    FILE *extended_file = kv_win32_fopen(joined, "wb");
    assert(extended_file);
    assert(fwrite("utf8", 1, 4, extended_file) == 4);
    assert(!fclose(extended_file));
    char *canonical_joined = kv_win32_realpath(joined);
    assert(canonical_joined && !strchr(canonical_joined, '/'));
    extended_file = kv_win32_fopen(joined, "rb");
    assert(extended_file);
    assert(fread(buf, 1, 4, extended_file) == 4 && !memcmp(buf, "utf8", 4));
    assert(!fclose(extended_file));
    wchar_t *joined_wide = kv_win32_utf16(canonical_joined);
    assert(DeleteFileW(joined_wide));
    free(joined_wide);
    free(canonical_joined);
    free(joined);
    free(canonical_root);
    char single[MAX_PATH];
    snprintf(single, sizeof(single), "%s\\single.txt", root);
    FILE *unicode_file = kv_win32_fopen(single, "wb");
    assert(unicode_file);
    assert(fwrite("one", 1, 3, unicode_file) == 3);
    assert(!fclose(unicode_file));
    assert(DeleteFileA(single));
    HANDLE host = CreateFileA(single, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(host != INVALID_HANDLE_VALUE);
    DWORD n;
    assert(WriteFile(host, "one", 3, &n, NULL));
    CloseHandle(host);
    s = fs_confined_init(single, FALSE);
    assert(s);
    assert(!s->fs_attach(s, &r, q, 0, "", ""));
    assert(!s->fs_open(s, q, r, P9_O_RDONLY, NULL, NULL));
    assert(s->fs_readdir(s, r, 0, buf, sizeof(buf)) == 34);
    assert(s->fs_readdir(s, r, 1, buf, sizeof(buf)) == 0);
    char *single_name[] = {"single.txt"};
    assert(s->fs_walk(s, &f, q, r, 1, single_name) == 1);
    assert(!s->fs_stat(s, f, &st) && (st.st_mode & 0111) == 0111);
    assert(!s->fs_open(s, q, f, P9_O_RDWR, NULL, NULL));
    assert(s->fs_read(s, f, 0, buf, 3) == 3 && !memcmp(buf, "one", 3));
    assert(s->fs_write(s, f, 0, (uint8_t *)"two", 3) == 3);
    assert(s->fs_unlinkat(s, r, "single.txt") == -P9_EPERM);
    s->fs_delete(s, f);
    s->fs_delete(s, r);
    s->fs_end(s);
    free(s);
    assert(DeleteFileA(single));
    DWORD before_handles, after_handles;
    assert(GetProcessHandleCount(GetCurrentProcess(), &before_handles));
    /* Poweroff releases outstanding fids and their pinned parent handles. */
    s = fs_confined_init(root, FALSE);
    assert(s);
    assert(!s->fs_attach(s, &r, q, 0, "", ""));
    assert(!s->fs_mkdir(s, q, r, "poweroff", 0700, 0));
    char *poweroff[] = {"poweroff"};
    assert(s->fs_walk(s, &f, q, r, 1, poweroff) == 1);
    assert(!s->fs_create(s, q, f, "live.txt", P9_O_RDWR, 0600, 0));
    s->fs_end(s);
    free(s);
    assert(GetProcessHandleCount(GetCurrentProcess(), &after_handles));
    assert(before_handles == after_handles);
    char live[MAX_PATH], powerdir[MAX_PATH];
    snprintf(powerdir, sizeof(powerdir), "%s\\poweroff", root);
    snprintf(live, sizeof(live), "%s\\poweroff\\live.txt", root);
    assert(DeleteFileA(live));
    assert(RemoveDirectoryA(powerdir));
    assert(RemoveDirectoryA(root));
    puts("win32 filesystem tests passed");
    return 0;
}
