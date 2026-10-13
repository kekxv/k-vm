#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Native private launch bundles. SPDX-License-Identifier: MIT */
#include "win32_files.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <winternl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "workspace_host.h"

static BOOL contains_path(const char *root, const char *path)
{
    wchar_t *r = kv_win32_utf16(root), *p = kv_win32_utf16(path);
    BOOL result = FALSE;
    if (r && p) {
        size_t n = wcslen(r);
        while (n && (r[n - 1] == L'\\' || r[n - 1] == L'/')) n--;
        result = wcslen(p) >= n && CompareStringOrdinal(r, n, p, n, TRUE) == CSTR_EQUAL &&
            (!p[n] || p[n] == L'\\' || p[n] == L'/');
    }
    free(r); free(p); return result;
}
static PSECURITY_DESCRIPTOR private_security(void)
{
    HANDLE token;
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &n);
    TOKEN_USER *user = malloc(n);
    wchar_t *sid = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (user && GetTokenInformation(token, TokenUser, user, n, &n) &&
        ConvertSidToStringSidW(user->User.Sid, &sid)) {
        size_t len = wcslen(sid) + 80;
        wchar_t *sddl = malloc(len * sizeof(*sddl));
        if (sddl) {
            swprintf(sddl, len, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%ls)", sid);
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL);
            free(sddl);
        }
    }
    if (sid) LocalFree(sid);
    free(user); CloseHandle(token); return sd;
}
char *workspace_host_create_bundle(VirtMachineParams *p, const char *cwd)
{
    char *parents[4] = { kv_win32_getenv_utf8("TEMP"), kv_win32_getenv_utf8("TMP"),
        kv_win32_getenv_utf8("LOCALAPPDATA"), kv_win32_getenv_utf8("USERPROFILE") };
    PSECURITY_DESCRIPTOR sd = private_security();
    char *result = NULL, *canonical_cwd = cwd ? kv_win32_realpath(cwd) : NULL;
    if (!sd || (cwd && !canonical_cwd)) goto done;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    for (int i = 0; i < 4 && !result; i++) {
        char *parent = parents[i] ? kv_win32_realpath(parents[i]) : NULL;
        if (!parent) continue;
        BOOL overlap = canonical_cwd && contains_path(canonical_cwd, parent);
        for (int j = 0; j < p->fs_count && !overlap; j++) {
            if (!p->tab_fs[j].read_only) {
                char *root = kv_win32_realpath(p->tab_fs[j].filename);
                overlap = !root || contains_path(root, parent);
                free(root);
            }
        }
        if (!overlap) {
            /* Random names and exclusive creation prevent precreated bundles. */
            for (unsigned attempt = 0; attempt < 128; attempt++) {
                unsigned char random[16]; char suffix[33];
                if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) break;
                for (int j = 0; j < 16; j++) snprintf(suffix + 2 * j, 3, "%02x", random[j]);
                char *path;
                if (asprintf(&path, "%s\\k-vm-launch-%s", parent, suffix) < 0) abort();
                wchar_t *w = kv_win32_utf16(path);
                BOOL made = w && CreateDirectoryW(w, &sa);
                DWORD error = GetLastError();
                free(w);
                if (made) {
                    HANDLE h = kv_win32_open(path, WRITE_DAC | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             OPEN_EXISTING, 0);
                    BOOL present, defaulted; PACL acl;
                    DWORD acl_error = ERROR_ACCESS_DENIED;
                    if (h != INVALID_HANDLE_VALUE && GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && present)
                        acl_error = SetSecurityInfo(h, SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            NULL, NULL, acl, NULL);
                    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                    if (acl_error == ERROR_SUCCESS) { result = path; break; }
                    workspace_host_cleanup(path);
                }
                free(path);
                if (error != ERROR_ALREADY_EXISTS) break;
            }
        }
        free(parent);
    }
done:
    for (int i = 0; i < 4; i++) free(parents[i]);
    free(canonical_cwd);
    if (sd) LocalFree(sd);
    if (!result) fprintf(stderr, "k-vm sandbox: no private launch directory outside writable mappings; narrow the mapped directory\n");
    return result;
}
WorkspaceDir workspace_host_open_dir(const char *path)
{
    HANDLE h = kv_win32_open(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    BY_HANDLE_FILE_INFORMATION info;
    if (h != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(h, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(h); errno = ENOTDIR; return -1;
    }
    return (WorkspaceDir)h;
}
void workspace_host_close_dir(WorkspaceDir dir) { CloseHandle((HANDLE)dir); }
int workspace_host_copy_config(WorkspaceDir source, WorkspaceDir dest, const char *name)
{
    if (strcmp(name, "auth.json") && strcmp(name, "settings.json") && strcmp(name, "models.json")) {
        errno = EACCES; return -1;
    }
    KVWin32File in, out;
    if (kv_win32_relative((HANDLE)source, name, GENERIC_READ, FILE_SHARE_READ,
                         OPEN_EXISTING, 0, &in)) return errno == ENOENT ? 0 : -1;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(in.handle, &size) || size.QuadPart > 4 * 1024 * 1024) {
        kv_win32_file_close(&in); errno = EFBIG; return -1;
    }
    if (kv_win32_relative((HANDLE)dest, name, GENERIC_WRITE, 0, CREATE_NEW, 0, &out)) {
        kv_win32_file_close(&in); return -1;
    }
    char buf[8192]; DWORD n, done, written; size_t total = 0; int status = 0;
    for (;;) {
        if (!ReadFile(in.handle, buf, sizeof(buf), &n, NULL)) { status = -1; break; }
        if (!n) break;
        total += n;
        if (total > 4 * 1024 * 1024) { errno = EFBIG; status = -1; break; }
        for (done = 0; done < n; done += written) {
            if (!WriteFile(out.handle, buf + done, n - done, &written, NULL) || !written) {
                status = -1; break;
            }
        }
        if (status) break;
    }
    kv_win32_file_close(&in); kv_win32_file_close(&out); return status;
}
BOOL workspace_host_update_succeeded(const char *bundle)
{
    WorkspaceDir dir = workspace_host_open_dir(bundle);
    if (dir == -1) return FALSE;
    KVWin32File file;
    int status = kv_win32_relative((HANDLE)dir, "result/status", GENERIC_READ,
                                  FILE_SHARE_READ, OPEN_EXISTING, 0, &file);
    BOOL success = FALSE;
    if (!status) {
        char bytes[16]; DWORD n;
        success = ReadFile(file.handle, bytes, sizeof(bytes), &n, NULL) && n == 2 &&
            bytes[0] == '0' && bytes[1] == '\n';
        kv_win32_file_close(&file);
    }
    workspace_host_close_dir(dir); return success;
}
/* Open a single cleanup child without following a reparse point. The handle
 * remains relative to the pinned directory even if a filename is replaced. */
static HANDLE cleanup_child(HANDLE parent, const wchar_t *name)
{
    typedef NTSTATUS (NTAPI *CreateFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    CreateFn create = (CreateFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    if (!create) return INVALID_HANDLE_VALUE;
    UNICODE_STRING us = { (USHORT)(wcslen(name) * 2), (USHORT)(wcslen(name) * 2), (PWSTR)name };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa); oa.RootDirectory = parent; oa.ObjectName = &us; oa.Attributes = 0x40;
    IO_STATUS_BLOCK io; HANDLE h;
    NTSTATUS status = create(&h, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        1, 0x00200000 | 0x20, NULL, 0);
    return status < 0 ? INVALID_HANDLE_VALUE : h;
}
static void cleanup_directory(HANDLE parent)
{
    uint8_t buffer[65536];
    BOOL restart = TRUE;
    while (GetFileInformationByHandleEx(parent,
            restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
            buffer, sizeof(buffer))) {
        restart = FALSE;
        FILE_ID_BOTH_DIR_INFO *entry = (FILE_ID_BOTH_DIR_INFO *)buffer;
        for (;;) {
            size_t n = entry->FileNameLength / sizeof(wchar_t);
            wchar_t *name = malloc((n + 1) * sizeof(*name));
            if (!name) return;
            memcpy(name, entry->FileName, entry->FileNameLength); name[n] = 0;
            if (wcscmp(name, L".") && wcscmp(name, L"..")) {
                HANDLE child = cleanup_child(parent, name);
                if (child != INVALID_HANDLE_VALUE) {
                    BY_HANDLE_FILE_INFORMATION info;
                    if (GetFileInformationByHandle(child, &info) &&
                        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) cleanup_directory(child);
                    FILE_DISPOSITION_INFO dispose = { TRUE };
                    SetFileInformationByHandle(child, FileDispositionInfo, &dispose, sizeof(dispose));
                    CloseHandle(child);
                }
            }
            free(name);
            if (!entry->NextEntryOffset) break;
            entry = (FILE_ID_BOTH_DIR_INFO *)((uint8_t *)entry + entry->NextEntryOffset);
        }
    }
}
void workspace_host_cleanup(const char *bundle)
{
    HANDLE dir = kv_win32_open(bundle, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    if (dir == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(dir, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        cleanup_directory(dir);
        FILE_DISPOSITION_INFO dispose = { TRUE };
        SetFileInformationByHandle(dir, FileDispositionInfo, &dispose, sizeof(dispose));
    }
    CloseHandle(dir);
}
int workspace_host_mkdir(const char *path)
{
    wchar_t *w = kv_win32_utf16(path);
    if (!w) return -1;
    for (wchar_t *q = w; *q; q++) if (*q == L'/') *q = L'\\';
    BOOL ok = CreateDirectoryW(w, NULL); DWORD error = GetLastError(); free(w);
    return ok ? 0 : kv_win32_error(error);
}
char *workspace_host_cwd(void)
{
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w = malloc(n * sizeof(*w));
    if (!w || !GetCurrentDirectoryW(n, w)) { free(w); return NULL; }
    char *s = kv_win32_utf8(w); free(w); return s;
}
unsigned long workspace_host_pid(void) { return GetCurrentProcessId(); }
char *workspace_host_home(void)
{
    char *home = kv_win32_getenv_utf8("USERPROFILE");
    if (!home) home = kv_win32_getenv_utf8("HOME");
    return home;
}
FILE *workspace_host_script(const char *path)
{
    char *native = strdup(path);
    if (!native) return NULL;
    for (char *q = native; *q; q++) if (*q == '/') *q = '\\';
    FILE *file = kv_win32_fopen(native, "wb"); free(native); return file;
}
