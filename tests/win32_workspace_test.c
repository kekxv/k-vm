/* Native Windows workspace contract tests. SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "win32_files.h"
#include "workspace_host.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <aclapi.h>
#include <winternl.h>
static char *join(const char *root, const char *name)
{
    char *result; assert(asprintf(&result, "%s/%s", root, name) >= 0); return result;
}
static void write_text(const char *path, const char *text)
{
    FILE *file = workspace_host_script(path); assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text)); assert(!fclose(file));
}
int main(void)
{
    VirtMachineParams p = {0};
    char *bundle = workspace_host_create_bundle(&p, NULL); assert(bundle);
    HANDLE acl_handle = kv_win32_open(bundle, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    OPEN_EXISTING, 0);
    assert(acl_handle != INVALID_HANDLE_VALUE);
    /* Helpers inspect metadata even when the caller only requests ACL access. */
    typedef NTSTATUS (NTAPI *QueryFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    PUBLIC_OBJECT_BASIC_INFORMATION basic;
    QueryFn query = (QueryFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    assert(query && query(acl_handle, 0, &basic, sizeof(basic), NULL) >= 0);
    assert(basic.GrantedAccess & FILE_READ_ATTRIBUTES);
    PSECURITY_DESCRIPTOR sd = NULL; PACL acl = NULL;
    assert(GetSecurityInfo(acl_handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                          NULL, NULL, &acl, NULL, &sd) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control; DWORD revision;
    assert(GetSecurityDescriptorControl(sd, &control, &revision));
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        puts("Wine does not persist native protected DACLs; ACL persistence requires native Windows verification");
    } else {
        assert(control & SE_DACL_PROTECTED);
        assert(acl && acl->AceCount == 2);
    }
    LocalFree(sd); CloseHandle(acl_handle);
    char *dir = join(bundle, "配置"); assert(!workspace_host_mkdir(dir));
    char *file = join(dir, "auth.json"); write_text(file, "{\"key\":\"secret\"}\n");
    char *dest_path = join(bundle, "copy"); assert(!workspace_host_mkdir(dest_path));
    WorkspaceDir source = workspace_host_open_dir(dir), dest = workspace_host_open_dir(dest_path);
    assert(source != -1 && dest != -1);
    assert(!workspace_host_copy_config(source, dest, "auth.json"));
    assert(!workspace_host_copy_config(source, dest, "models.json"));
    assert(workspace_host_copy_config(source, dest, "../auth.json") == -1);
    assert(workspace_host_copy_config(source, dest, "other.json") == -1);
    char *big = join(dir, "settings.json");
    HANDLE h = kv_win32_open(big, GENERIC_WRITE, 0, CREATE_NEW, 0); assert(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size; size.QuadPart = 4 * 1024 * 1024 + 1;
    assert(SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h)); CloseHandle(h);
    assert(workspace_host_copy_config(source, dest, "settings.json") == -1);
    workspace_host_close_dir(source); workspace_host_close_dir(dest);
    char *result_dir = join(bundle, "result"); assert(!workspace_host_mkdir(result_dir));
    char *status = join(result_dir, "status");
    write_text(status, "0\n"); assert(workspace_host_update_succeeded(bundle));
    write_text(status, "0\r\n"); assert(!workspace_host_update_succeeded(bundle));
    write_text(status, "0\nextra"); assert(!workspace_host_update_succeeded(bundle));
    /* Unicode environment values survive without the active ANSI code page. */
    assert(SetEnvironmentVariableW(L"KV_WORKSPACE_TEST", L"配置"));
    char *env = kv_win32_getenv_utf8("KV_WORKSPACE_TEST"); assert(env && !strcmp(env, "配置")); free(env);
    /* A writable drive root makes all native temp candidates unsafe. */
    char *root = kv_win32_realpath(bundle); assert(root);
    char drive[4] = { root[4], ':', '\\', 0 };
    p.fs_count = 1; p.tab_fs[0].filename = drive; p.tab_fs[0].read_only = FALSE;
    assert(!workspace_host_create_bundle(&p, NULL));
    VirtMachineParams empty = {0};
    char *outside = workspace_host_create_bundle(&empty, NULL); assert(outside);
    char *sentinel = join(outside, "keep"); write_text(sentinel, "safe");
    char *link = join(bundle, "outside-link");
    wchar_t *wl = kv_win32_utf16(link), *wo = kv_win32_utf16(outside);
    if (CreateSymbolicLinkW(wl, wo, SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        assert(workspace_host_open_dir(link) == -1);
    }
    free(wl); free(wo);
    workspace_host_cleanup(bundle);
    HANDLE keep = kv_win32_open(sentinel, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    assert(keep != INVALID_HANDLE_VALUE); char safe[4]; DWORD got;
    assert(ReadFile(keep, safe, sizeof(safe), &got, NULL) && got == 4 && !memcmp(safe, "safe", 4));
    CloseHandle(keep);
    workspace_host_cleanup(outside); free(outside); free(sentinel); free(link);
    assert(workspace_host_open_dir(bundle) == -1);
    free(root); free(status); free(result_dir); free(big); free(dest_path); free(file); free(dir); free(bundle);
    puts("Win32 workspace tests passed"); return 0;
}
