/* On-demand installation and disk-specific readiness hints. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "initialize.h"
#ifdef _WIN32
#include "win32_files.h"
#else
#include <signal.h>
#include <sys/wait.h>
#endif

static int fingerprint(const char *disk, char *text, size_t size)
{
#ifdef _WIN32
    HANDLE f = kv_win32_open(disk, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            OPEN_EXISTING, 0);
    if (f == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION i;
    BOOL ok = GetFileInformationByHandle(f, &i);
    CloseHandle(f);
    if (!ok) return -1;
    snprintf(text, size, "%lu:%lu:%lu:%lu:%lu:%lu:%lu\n",
             (unsigned long)i.dwVolumeSerialNumber, (unsigned long)i.nFileIndexHigh,
             (unsigned long)i.nFileIndexLow, (unsigned long)i.nFileSizeHigh,
             (unsigned long)i.nFileSizeLow, (unsigned long)i.ftLastWriteTime.dwHighDateTime,
             (unsigned long)i.ftLastWriteTime.dwLowDateTime);
#else
    struct stat s;
    if (stat(disk, &s)) return -1;
#ifdef __APPLE__
    long ns = s.st_mtimespec.tv_nsec;
#else
    long ns = s.st_mtim.tv_nsec;
#endif
    snprintf(text, size, "%llu:%llu:%llu:%lld:%ld:%lld\n",
             (unsigned long long)s.st_dev, (unsigned long long)s.st_ino,
             (unsigned long long)s.st_size, (long long)s.st_mtime, ns, (long long)s.st_ctime);
#endif
    return 0;
}

static int readiness(const char *disk, BOOL write_stamp)
{
    char expected[256], saved[256], *path;
    if (fingerprint(disk, expected, sizeof(expected))) return -1;
    if (asprintf(&path, "%s.pi-ready", disk) < 0) abort();
    int status = -1;
#ifdef _WIN32
    HANDLE f = kv_win32_open(path, write_stamp ? GENERIC_WRITE | FILE_READ_ATTRIBUTES : GENERIC_READ,
                            FILE_SHARE_READ, write_stamp ? OPEN_ALWAYS : OPEN_EXISTING, 0);
    free(path);
    if (f == INVALID_HANDLE_VALUE) return -1;
    DWORD n;
    if (write_stamp) {
        BY_HANDLE_FILE_INFORMATION info;
        size_t len = strlen(expected);
        if (GetFileInformationByHandle(f, &info) && info.nNumberOfLinks == 1 &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && SetEndOfFile(f) &&
            WriteFile(f, expected, len, &n, NULL) && n == len && FlushFileBuffers(f)) status = 0;
    } else if (ReadFile(f, saved, sizeof(saved), &n, NULL) && n == strlen(expected) &&
               !memcmp(saved, expected, n)) status = 0;
    CloseHandle(f);
#else
    int f = open(path, (write_stamp ? O_WRONLY | O_CREAT : O_RDONLY) |
                       O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    free(path);
    if (f < 0) return -1;
    struct stat s;
    if (!fstat(f, &s) && S_ISREG(s.st_mode) && (!write_stamp || s.st_nlink == 1)) {
        size_t len = strlen(expected);
        if (write_stamp) {
            if (!ftruncate(f, 0) && write(f, expected, len) == (ssize_t)len && !fsync(f)) status = 0;
        } else if (read(f, saved, sizeof(saved)) == (ssize_t)len && !memcmp(saved, expected, len))
            status = 0;
    }
    close(f);
#endif
    return status;
}

BOOL sandbox_pi_initialized(const char *disk) { return readiness(disk, FALSE) == 0; }
int sandbox_pi_record_initialized(const char *disk) { return readiness(disk, TRUE); }

#ifndef _WIN32
static volatile sig_atomic_t installer_pid;
static volatile sig_atomic_t installer_interrupted;
static void cancel_installer(int number)
{
    installer_interrupted = 1;
    if (installer_pid > 0) kill(installer_pid, number);
}
#endif

static int run_command(const char *const *args, BOOL self)
{
#ifdef _WIN32
    /* CreateProcess needs Microsoft argv quoting; never invoke a command shell. */
    size_t length = 1;
    for (int i = 0; args[i]; i++) length += strlen(args[i]) * 2 + 4;
    char *command = malloc(length), *out = command;
    if (!command) return 1;
    for (int i = 0; args[i]; i++) {
        *out++ = '"';
        size_t slashes = 0;
        for (const char *q = args[i]; ; q++) {
            if (*q == '\\') { slashes++; continue; }
            size_t n = (*q == '"' || !*q) ? 2 * slashes : slashes;
            while (n--) *out++ = '\\';
            slashes = 0;
            if (!*q) break;
            if (*q == '"') *out++ = '\\';
            *out++ = *q;
        }
        *out++ = '"'; *out++ = ' ';
    }
    *out = 0;
    wchar_t module[32768];
    DWORD n = self ? GetModuleFileNameW(NULL, module, 32768) : 1;
    wchar_t *application = self ? NULL : kv_win32_utf16(args[0]);
    wchar_t *wide = kv_win32_utf16(command);
    free(command);
    if (!n || n >= 32768 || !wide || (!self && !application)) { free(wide); free(application); return 1; }
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    BOOL ok = CreateProcessW(self ? module : application, wide, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    DWORD error = ok ? 0 : GetLastError();
    free(application);
    free(wide);
    if (!ok) return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 127 : 1;
    DWORD status = 1;
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return (int)status;
#else
    (void)self;
    struct sigaction action = {0}, old_int, old_term;
    action.sa_handler = cancel_installer;
    sigemptyset(&action.sa_mask);
    installer_interrupted = 0;
    sigaction(SIGINT, &action, &old_int); sigaction(SIGTERM, &action, &old_term);
    pid_t child = fork();
    if (!child) {
        sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
        execvp(args[0], (char *const *)args);
        int missing = errno == ENOENT;
        if (!missing) perror("k-vm init: exec");
        _exit(missing ? 127 : 1);
    }
    installer_pid = child > 0 ? child : 0;
    if (installer_interrupted && child > 0) kill(child, SIGTERM);
    int status = 0;
    pid_t waited = -1;
    if (child > 0) do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    installer_pid = 0;
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
    return waited == child && child > 0 && !installer_interrupted && WIFEXITED(status)
           ? WEXITSTATUS(status) : 1;
#endif
}

int sandbox_initialize_image(const char *executable, const VirtMachineParams *p)
{
    char memory[32], network[32];
    snprintf(memory, sizeof(memory), "%llu", (unsigned long long)(p->ram_size >> 20));
    if (p->eth_count && !strcmp(p->tab_eth[0].driver, "tap"))
        snprintf(network, sizeof(network), "tap:%s", p->tab_eth[0].ifname);
    else strcpy(network, "user");
    const char *args[] = {executable, "init", "--image", p->cfg_filename,
                          "--agent", "pi", "--memory", memory, "--network", network, NULL};
    fputs("k-vm: initializing Node.js and Pi in the local image (first use)...\n", stderr);
    return run_command(args, TRUE);
}

int sandbox_prefetch_node(const char *version, const char *destination)
{
    if (!version || !*version || strlen(version) > 32) return 1;
    for (const char *v = version; *v; v++) if ((*v < '0' || *v > '9') && *v != '.') return 1;
    char *curl_program;
#ifdef _WIN32
    wchar_t system[32768];
    UINT n = GetSystemDirectoryW(system, 32768);
    if (!n || n + 10 >= 32768) return 127;
    wcscat(system, L"\\curl.exe");
    curl_program = kv_win32_utf8(system);
    if (!curl_program) return 1;
#else
    curl_program = strdup("curl");
#endif
    if (!curl_program) return 1;
    char archive[96];
    snprintf(archive, sizeof(archive), "node-v%s-linux-riscv64.tar.xz", version);
    const char *names[] = {archive, "SHASUMS256.txt"};
    int status = 0;
    for (int i = 0; i < 2; i++) {
        char *url, *output;
        if (asprintf(&url, "https://unofficial-builds.nodejs.org/download/release/v%s/%s", version, names[i]) < 0 ||
            asprintf(&output, "%s/%s", destination, names[i]) < 0) abort();
        const char *args[] = {curl_program, "--fail", "--location", "--proto", "=https",
                             "--proto-redir", "=https", "--retry", "3", "--connect-timeout", "30",
                             "--max-time", "300", "--output", output, url, NULL};
        status = run_command(args, FALSE);
        free(url); free(output);
        if (status) break;
    }
    free(curl_program);
    return status;
}
