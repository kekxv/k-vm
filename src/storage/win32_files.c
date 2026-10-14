/* Handle-relative, reparse-resistant native Windows file access.
 * SPDX-License-Identifier: MIT */
#include "win32_files.h"
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winternl.h>
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 1
#define FILE_NON_DIRECTORY_FILE 0x40
#endif
int kv_win32_error(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_CANT_ACCESS_FILE:
        errno = EACCES;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        errno = ENOSYS;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}
wchar_t *kv_win32_utf16(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (!n) {
        kv_win32_error(GetLastError());
        return NULL;
    }
    wchar_t *p = malloc(n * sizeof(*p));
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, p, n)) {
        free(p);
        return NULL;
    }
    return p;
}
char *kv_win32_utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, NULL, 0,
                                NULL, NULL);
    if (!n) {
        kv_win32_error(GetLastError());
        return NULL;
    }
    char *p = malloc(n);
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, p, n, NULL,
                             NULL)) {
        free(p);
        return NULL;
    }
    return p;
}
int kv_win32_valid_name(const char *s) {
    if (!s || !*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    size_t n = strlen(s);
    if (s[n - 1] == '.' || s[n - 1] == ' ')
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || strchr("/\\:<>\"|?*", s[i]))
            return 0;
    char stem[16];
    size_t k = strcspn(s, ".");
    while (k && s[k - 1] == ' ')
        k--;
    if (k >= sizeof(stem))
        return 1;
    for (size_t i = 0; i < k; i++)
        stem[i] = toupper((unsigned char)s[i]);
    stem[k] = 0;
    if (!strcmp(stem, "CON") || !strcmp(stem, "PRN") || !strcmp(stem, "AUX") ||
        !strcmp(stem, "NUL") || !strcmp(stem, "CONIN$") ||
        !strcmp(stem, "CONOUT$"))
        return 0;
    if (k == 4 && (!memcmp(stem, "COM", 3) || !memcmp(stem, "LPT", 3)) &&
        stem[3] >= '1' && stem[3] <= '9')
        return 0;
    /* Windows also recognizes superscript 1, 2, 3 as device numbers. */
    if (k == 5 && (!memcmp(stem, "COM", 3) || !memcmp(stem, "LPT", 3)) &&
        (unsigned char)stem[3] == 0xc2 &&
        ((unsigned char)stem[4] == 0xb9 || (unsigned char)stem[4] == 0xb2 ||
         (unsigned char)stem[4] == 0xb3))
        return 0;
    return 1;
}
static int checked(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        errno = EACCES;
        return 0;
    }
    return 1;
}
static HANDLE relative_one(HANDLE root, const wchar_t *name, DWORD access,
                           DWORD share, DWORD disposition, int dir) {
    typedef NTSTATUS(NTAPI * CreateFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                       PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                       ULONG, ULONG, ULONG, PVOID, ULONG);
    typedef ULONG(WINAPI * ErrorFn)(NTSTATUS);
    CreateFn fn = (CreateFn)(void *)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    ErrorFn err = (ErrorFn)(void *)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
    if (!fn || !err) {
        errno = ENOSYS;
        return INVALID_HANDLE_VALUE;
    }
    if (wcslen(name) > USHRT_MAX / sizeof(wchar_t)) {
        errno = ENAMETOOLONG;
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING us;
    us.Buffer = (PWSTR)name;
    us.Length = (USHORT)(wcslen(name) * 2);
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.RootDirectory = root;
    oa.ObjectName = &us;
    oa.Attributes = 0x40;
    ULONG disp = disposition == CREATE_NEW          ? 2
                 : disposition == CREATE_ALWAYS     ? 5
                 : disposition == OPEN_ALWAYS       ? 3
                 : disposition == TRUNCATE_EXISTING ? 4
                                                    : 1;
    HANDLE h;
    IO_STATUS_BLOCK io;
    /* checked() queries metadata; native Windows enforces this access bit. */
    NTSTATUS st = fn(&h, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL,
                     FILE_ATTRIBUTE_NORMAL, share, disp,
                     FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT |
                         (dir == 1   ? FILE_DIRECTORY_FILE
                          : dir == 0 ? FILE_NON_DIRECTORY_FILE
                                     : 0),
                     NULL, 0);
    if (st < 0) {
        kv_win32_error(err(st));
        return INVALID_HANDLE_VALUE;
    }
    if (!checked(h)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
void kv_win32_file_close(KVWin32File *f) {
    if (f->handle != INVALID_HANDLE_VALUE)
        CloseHandle(f->handle);
    for (size_t i = f->count; i; i--)
        CloseHandle(f->parents[i - 1]);
    free(f->parents);
    f->handle = INVALID_HANDLE_VALUE;
    f->parents = NULL;
    f->count = 0;
}
int kv_win32_relative(HANDLE root, const char *path, DWORD access, DWORD share,
                      DWORD disposition, int dir, KVWin32File *out) {
    memset(out, 0, sizeof(*out));
    out->handle = INVALID_HANDLE_VALUE;
    if (!*path) { /* Reopen rather than duplicate, so access rights belong to
                     this fid. */
        out->handle = relative_one(root, L"", access, share, disposition, dir);
        return out->handle == INVALID_HANDLE_VALUE ? -1 : 0;
    }
    char *copy = strdup(path);
    if (!copy) {
        errno = ENOMEM;
        return -1;
    }
    HANDLE parent = root;
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash)
            *slash = 0;
        if (!kv_win32_valid_name(part)) {
            errno = EACCES;
            goto fail;
        }
        wchar_t *w = kv_win32_utf16(part);
        if (!w)
            goto fail;
        HANDLE h = relative_one(
            parent, w,
            slash ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : access,
            slash ? FILE_SHARE_READ | FILE_SHARE_WRITE : share,
            slash ? OPEN_EXISTING : disposition, slash ? 1 : dir);
        free(w);
        if (h == INVALID_HANDLE_VALUE)
            goto fail;
        if (!slash) {
            out->handle = h;
            free(copy);
            return 0;
        }
        HANDLE *p = realloc(out->parents, (out->count + 1) * sizeof(*p));
        if (!p) {
            CloseHandle(h);
            errno = ENOMEM;
            goto fail;
        }
        out->parents = p;
        out->parents[out->count++] = h;
        parent = h;
        part = slash + 1;
    }
fail:
    free(copy);
    kv_win32_file_close(out);
    return -1;
}
HANDLE kv_win32_open(const char *path, DWORD access, DWORD share,
                     DWORD disposition, DWORD flags) {
    wchar_t *input = kv_win32_utf16(path);
    if (!input)
        return INVALID_HANDLE_VALUE;
    for (wchar_t *p = input; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    DWORD n = GetFullPathNameW(input, 0, NULL, NULL);
    wchar_t *full = malloc((n + 1) * sizeof(*full));
    if (!n || !full) {
        free(input);
        free(full);
        errno = EINVAL;
        return INVALID_HANDLE_VALUE;
    }
    if (!GetFullPathNameW(input, n + 1, full, NULL)) {
        DWORD e = GetLastError();
        free(input);
        free(full);
        kv_win32_error(e);
        return INVALID_HANDLE_VALUE;
    }
    free(input);
    size_t start = 0;
    if (!wcsncmp(full, L"\\\\?\\UNC\\", 8))
        start = 8;
    else if (!wcsncmp(full, L"\\\\?\\", 4) && full[5] == L':' &&
             full[6] == L'\\')
        start = 7;
    else if (full[1] == L':' && full[2] == L'\\')
        start = 3;
    else if (full[0] == L'\\' && full[1] == L'\\' && full[2] != L'.' &&
             full[2] != L'?')
        start = 2;
    else {
        free(full);
        errno = EACCES;
        return INVALID_HANDLE_VALUE;
    }
    if (start == 2 ||
        start == 8) { /* Pin the UNC share root before descending. */
        wchar_t *server = wcschr(full + start, L'\\');
        wchar_t *end = server ? wcschr(server + 1, L'\\') : NULL;
        if (!server) {
            free(full);
            errno = EINVAL;
            return INVALID_HANDLE_VALUE;
        }
        start = end ? (size_t)(end - full) + 1 : wcslen(full);
    }
    wchar_t saved = full[start];
    full[start] = 0;
    HANDLE root = CreateFileW(
        full, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    full[start] = saved;
    if (root == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(full);
        kv_win32_error(e);
        return root;
    }
    if (!checked(root)) {
        CloseHandle(root);
        free(full);
        return INVALID_HANDLE_VALUE;
    }
    char *relative = kv_win32_utf8(full + start);
    free(full);
    if (!relative) {
        CloseHandle(root);
        return INVALID_HANDLE_VALUE;
    }
    for (char *p = relative; *p; p++)
        if (*p == '\\')
            *p = '/';
    size_t len = strlen(relative);
    while (len && relative[len - 1] == '/')
        relative[--len] = 0;
    KVWin32File file;
    int result = kv_win32_relative(root, relative, access, share, disposition,
                                   -1, &file);
    free(relative);
    CloseHandle(root);
    if (result)
        return INVALID_HANDLE_VALUE;
    HANDLE h = file.handle;
    file.handle = INVALID_HANDLE_VALUE;
    kv_win32_file_close(&file);
    /* All helpers use synchronous handles; Windows still supports explicit byte
     * offsets through the OVERLAPPED argument on synchronous disk handles. */
    (void)flags;
    return h;
}

char *kv_win32_realpath(const char *path) {
    HANDLE h =
        kv_win32_open(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
    wchar_t *w = malloc((n + 1) * sizeof(*w));
    if (!n || !w) {
        CloseHandle(h);
        free(w);
        return NULL;
    }
    DWORD got = GetFinalPathNameByHandleW(h, w, n + 1, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!got || got > n) {
        free(w);
        return NULL;
    }
    char *s = kv_win32_utf8(w);
    free(w);
    return s;
}

int kv_win32_rename_handle(HANDLE source, HANDLE directory, const char *name) {
    typedef NTSTATUS(NTAPI * SetFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                    FILE_INFORMATION_CLASS);
    typedef ULONG(WINAPI * ErrorFn)(NTSTATUS);
    SetFn set = (SetFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                              "NtSetInformationFile");
    ErrorFn error = (ErrorFn)(void *)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
    if (!set || !error) {
        errno = ENOSYS;
        return -1;
    }
    if (!kv_win32_valid_name(name)) {
        errno = EACCES;
        return -1;
    }
    wchar_t *w = kv_win32_utf16(name);
    if (!w)
        return -1;
    size_t len = wcslen(w) * sizeof(*w),
           size = offsetof(FILE_RENAME_INFO, FileName) + len;
    FILE_RENAME_INFO *info = calloc(1, size);
    if (!info) {
        free(w);
        errno = ENOMEM;
        return -1;
    }
    /* FileRenameInformationEx uses ULONG Flags in place of BOOLEAN
     * ReplaceIfExists. POSIX replacement allows an open destination, and
     * IGNORE_READONLY_ATTRIBUTE preserves the destination's old metadata. */
    ULONG flags = 0x1 | 0x2 | 0x40;
    memcpy(info, &flags, sizeof(flags));
    info->RootDirectory = directory;
    info->FileNameLength = len;
    memcpy(info->FileName, w, len);
    IO_STATUS_BLOCK io;
    NTSTATUS status = set(source, &io, info, size, (FILE_INFORMATION_CLASS)65);
    if (status == (NTSTATUS)0xc0000003 || status == (NTSTATUS)0xc0000002 ||
        status == (NTSTATUS)0xc00000bb) {
        /* Older kernels/Wine may not implement the extended information class. */
        memset(info, 0, sizeof(flags));
        info->ReplaceIfExists = TRUE;
        status = set(source, &io, info, size, (FILE_INFORMATION_CLASS)10);
    }
    free(w);
    free(info);
    return status < 0 ? kv_win32_error(error(status)) : 0;
}

char *kv_win32_getenv_utf8(const char *key) {
    wchar_t *wkey = kv_win32_utf16(key);
    if (!wkey)
        return NULL;
    SetLastError(ERROR_SUCCESS);
    DWORD needed = GetEnvironmentVariableW(wkey, NULL, 0);
    if (!needed) {
        DWORD error = GetLastError();
        free(wkey);
        if (error == ERROR_SUCCESS)
            return strdup("");
        return NULL;
    }
    wchar_t *value = malloc(needed * sizeof(*value));
    if (!value) {
        free(wkey);
        errno = ENOMEM;
        return NULL;
    }
    value[0] = 0;
    SetLastError(ERROR_SUCCESS);
    DWORD got = GetEnvironmentVariableW(wkey, value, needed);
    DWORD env_error = GetLastError();
    free(wkey);
    if (!got && env_error != ERROR_SUCCESS) {
        free(value);
        return NULL;
    }
    if (got >= needed) {
        free(value);
        errno = EAGAIN;
        return NULL;
    }
    char *result = kv_win32_utf8(value);
    free(value);
    return result;
}
FILE *kv_win32_fopen(const char *path, const char *mode) {
    wchar_t *wpath = kv_win32_utf16(path), *wmode = kv_win32_utf16(mode);
    if (!wpath || !wmode) {
        free(wpath);
        free(wmode);
        return NULL;
    }
    /* Extended Windows paths do not accept POSIX separators. Core path
     * joins deliberately use slash on every host, so normalize before CRT. */
    for (wchar_t *p = wpath; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    FILE *file = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return file;
}
