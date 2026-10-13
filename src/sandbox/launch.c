/* Native local-image sandbox launch policy. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "launch.h"
#include "workspace.h"
#include "fs_utils.h"
#ifdef _WIN32
#include "win32_files.h"
#define realpath(path, ignored) kv_win32_realpath(path)
#endif

#define MIN_MEMORY_MB 16
#define MAX_MEMORY_MB 4096

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "k-vm sandbox: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return -1;
}

static BOOL local_path(const char *path)
{
    return path && *path && !strstr(path, "://");
}

static char *resolve_resource(const char *cfg, const char *path)
{
    if (!local_path(path)) {
        fail("local images only; remote resource rejected: %s", path ? path : "(null)");
        return NULL;
    }
    char *relative = get_file_path(cfg, path);
    char *absolute = realpath(relative, NULL);
    if (!absolute) fail("%s: %s", relative, strerror(errno));
    free(relative);
    return absolute;
}

static int load_regular(const char *path, uint64_t limit, uint8_t **buf, int *len)
{
#ifdef _WIN32
    HANDLE file = kv_win32_open(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
    if (file == INVALID_HANDLE_VALUE) return fail("%s: %s", path, strerror(errno));
    LARGE_INTEGER size;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) || info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (uint64_t)size.QuadPart > limit || size.QuadPart >= INT_MAX) {
        CloseHandle(file); return fail("%s: expected a nonempty regular file within size limit", path);
    }
    *buf = malloc((size_t)size.QuadPart + 1);
    if (!*buf) { CloseHandle(file); return fail("out of memory loading %s", path); }
    DWORD done = 0;
    while (done < (DWORD)size.QuadPart) {
        DWORD n;
        if (!ReadFile(file, *buf + done, (DWORD)size.QuadPart - done, &n, NULL) || !n) {
            free(*buf); *buf = NULL; CloseHandle(file); return fail("%s: incomplete read", path);
        }
        done += n;
    }
    CloseHandle(file); (*buf)[done] = 0; *len = done; return 0;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return fail("%s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > limit || st.st_size >= INT_MAX) {
        close(fd);
        return fail("%s: expected a nonempty regular file within size limit", path);
    }
    *buf = malloc((size_t)st.st_size + 1);
    if (!*buf) { close(fd); return fail("out of memory loading %s", path); }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, *buf + done, st.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(*buf); *buf = NULL; close(fd); return fail("%s: incomplete read", path); }
        done += n;
    }
    close(fd);
    (*buf)[done] = 0;
    *len = done;
    return 0;
#endif
}

static BOOL valid_tag(const char *tag)
{
    if (!*tag || strlen(tag) > 63) return FALSE;
    for (const char *p = tag; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return FALSE;
    }
    return TRUE;
}

static int add_mapping(VirtMachineParams *p, const char *spec)
{
    if (p->fs_count >= MAX_FS_DEVICE) return fail("at most %d mappings", MAX_FS_DEVICE);
    char *copy = strdup(spec);
    char *eq = strchr(copy, '=');
    if (!eq) { free(copy); return fail("mapping syntax: TAG=HOST_PATH[:ro|rw]"); }
    *eq++ = 0;
    if (!valid_tag(copy) || !*eq) { free(copy); return fail("invalid mapping tag or host path"); }
    for (int i = 0; i < p->fs_count; ++i) {
        if (!strcmp(p->tab_fs[i].tag, copy)) { free(copy); return fail("duplicate mapping tag"); }
    }
    BOOL ro = TRUE;
    size_t n = strlen(eq);
    if (n >= 3 && (!strcmp(eq + n - 3, ":ro") || !strcmp(eq + n - 3, ":rw"))) {
        ro = eq[n - 1] == 'o';
        eq[n - 3] = 0;
    }
    char *resolved = realpath(eq, NULL);
    if (!resolved) { fail("mapping %s: %s", eq, strerror(errno)); free(copy); return -1; }
#ifdef _WIN32
    HANDLE mapping = kv_win32_open(resolved, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (mapping != INVALID_HANDLE_VALUE) CloseHandle(mapping);
    if (mapping == INVALID_HANDLE_VALUE) {
#else
    struct stat st;
    if (stat(resolved, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
#endif
        free(resolved); free(copy); return fail("mapping must be a regular file or directory");
    }
    VMFSEntry *entry = &p->tab_fs[p->fs_count++];
    *entry = (VMFSEntry){.tag = strdup(copy), .filename = resolved, .confined = TRUE, .read_only = ro};
    free(copy);
    return 0;
}

static int check_raw_disk(const char *path)
{
#ifdef _WIN32
    HANDLE file = kv_win32_open(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
    if (file == INVALID_HANDLE_VALUE) return fail("%s: %s", path, strerror(errno));
    LARGE_INTEGER size; DWORD n; uint8_t magic[4]; BY_HANDLE_FILE_INFORMATION info;
    int result = 0;
    if (!GetFileInformationByHandle(file, &info) || info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart % 512)
        result = fail("%s: disk must be a nonempty sector-aligned raw image", path);
    else if (!ReadFile(file, magic, sizeof(magic), &n, NULL) || n != sizeof(magic))
        result = fail("%s: unable to read disk header", path);
    else if (!memcmp(magic, "QFI\xfb", 4) || !memcmp(magic, "KDMV", 4) || !memcmp(magic, "vhdx", 4))
        result = fail("%s: compressed/container images unsupported; convert to raw first", path);
    CloseHandle(file); return result;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return fail("%s: %s", path, strerror(errno));
    struct stat st;
    uint8_t magic[4] = {0};
    int result = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % 512)
        result = fail("%s: disk must be a nonempty sector-aligned raw image", path);
    else if (pread(fd, magic, sizeof(magic), 0) != sizeof(magic))
        result = fail("%s: unable to read disk header", path);
    else if (!memcmp(magic, "QFI\xfb", 4) || !memcmp(magic, "KDMV", 4) || !memcmp(magic, "vhdx", 4))
        result = fail("%s: compressed/container images unsupported; convert to raw first", path);
    close(fd);
    return result;
#endif
}

static int check_image_capabilities(const char *cfg, const char *agent)
{
    char *metadata = get_file_path(cfg, "sandbox.json");
    if (access(metadata, F_OK)) { free(metadata); return 0; }
    uint8_t *buf; int len;
    if (load_regular(metadata, 65536, &buf, &len)) { free(metadata); return -1; }
    free(metadata);
    JSONValue caps = json_parse_value_len((char *)buf, len);
    free(buf);
    if (json_is_error(caps)) { json_free(caps); return fail("invalid sandbox image metadata"); }
    JSONValue agents = json_object_get(caps, "agents");
    BOOL found = FALSE;
    if (agents.type == JSON_ARRAY) {
        for (int i = 0; i < agents.u.array->len; ++i) {
            const char *name = json_get_str(agents.u.array->tab[i]);
            if (name && !strcmp(name, agent)) found = TRUE;
        }
    }
    JSONValue kvm = json_object_get(caps, "requires_kvm");
    BOOL needs_kvm = kvm.type == JSON_BOOL && kvm.u.b;
    json_free(caps);
    if (!found) return fail("selected program is not installed in this image; prepare the RISC-V Pi image or choose --agent shell");
    if (needs_kvm) return fail("KVM images are unsupported; use a RISC-V image");
    return 0;
}

static void help(void)
{
    puts("usage: k-vm sandbox [--image DIR_OR_CFG] [options] [-- agent arguments]\n"
         "  --agent shell|pi      default shell; cwd is mounted at /work\n"
         "  --config-dir DIR           only import selected agent config files\n"
         "  --no-workspace             legacy manual VM startup, no auto exports\n"
         "  --map TAG=HOST_PATH[:ro|rw]  expose directory or file over 9P (default ro)\n"
         "  --network user|none|tap:IF  default user networking\n"
         "  --disk-mode snapshot|ro|rw default snapshot; writes discarded on exit\n"
         "  --memory MB                16..4096 MB (overrides image config)\n"
         "  --append CMDLINE           append guest kernel arguments\n"
         "  --help                     show sandbox usage\n"
         "Directory images contain vm.cfg and local boot assets/raw disks.\n"
         "Ctrl+C is sent to the guest; Ctrl+A then X exits the sandbox.\n"
         "Mount TAG inside guest: mount -t 9p -o trans=virtio,version=9p2000.L TAG /mnt");
}

int sandbox_prepare(int argc, char **argv, VirtMachineParams *p,
                    BlockDeviceModeEnum *disk_mode, BOOL *allow_ctrlc)
{
    static const struct option opts[] = {
        {"agent", required_argument, NULL, 'g'}, {"config-dir", required_argument, NULL, 'c'},
        {"no-workspace", no_argument, NULL, 'l'},
        {"image", required_argument, NULL, 'i'}, {"map", required_argument, NULL, 'v'},
        {"network", required_argument, NULL, 'n'}, {"disk-mode", required_argument, NULL, 'd'},
        {"memory", required_argument, NULL, 'm'}, {"append", required_argument, NULL, 'a'},
        {"version", required_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *image = NULL, *network = "user", *cmdline = NULL;
    const char *maps[MAX_FS_DEVICE];
    int count = 0, memory = 0, opt;
    BOOL auto_workspace = TRUE;
    BOOL update = !strcmp(argv[0], "update");
    const char *pi_version = "latest";
    const char *agent = "shell", *config_dir = NULL;
    char *default_image = NULL;
    *disk_mode = BF_MODE_SNAPSHOT;
    *allow_ctrlc = FALSE;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "+", opts, NULL)) != -1) {
        if (update && opt != 'i' && opt != 'V' && opt != 'm' && opt != 'n' && opt != 'h')
            return fail("update accepts --image, --version, --memory and --network only");
        switch (opt) {
        case 'g': agent = optarg; break;
        case 'V': pi_version = optarg; break;
        case 'c': config_dir = optarg; break;
        case 'l': auto_workspace = FALSE; break;
        case 'i': image = optarg; break;
        case 'v':
            if (count >= MAX_FS_DEVICE) return fail("at most %d mappings", MAX_FS_DEVICE);
            maps[count++] = optarg; break;
        case 'n': network = optarg; break;
        case 'd':
            if (!strcmp(optarg, "snapshot")) *disk_mode = BF_MODE_SNAPSHOT;
            else if (!strcmp(optarg, "ro")) *disk_mode = BF_MODE_RO;
            else if (!strcmp(optarg, "rw")) *disk_mode = BF_MODE_RW;
            else return fail("invalid disk mode; choose snapshot, ro or rw");
            break;
        case 'm': {
            char *end;
            errno = 0;
            unsigned long mb = strtoul(optarg, &end, 10);
            if (errno || !*optarg || *end || mb < MIN_MEMORY_MB || mb > MAX_MEMORY_MB)
                return fail("memory must be %d..%d MB", MIN_MEMORY_MB, MAX_MEMORY_MB);
            memory = mb; break;
        }
        case 'a': cmdline = optarg; break;
        case 'h':
            if (update) puts("usage: k-vm update [--image DIR_OR_CFG] [--version latest|X.Y.Z] [--memory MB] [--network user|tap:IF]\nUpdates Pi in a cloned disk; commits on success. No project or credentials exported.");
            else help();
            exit(0);
        default: return fail("invalid sandbox arguments; see --help");
        }
    }
    if (update) {
        if (count || config_dir || !auto_workspace || cmdline || optind != argc || *disk_mode != BF_MODE_SNAPSHOT || !strcmp(network,"none"))
            return fail("update accepts --image, --version, --memory and network options only");
        agent = "pi";
        *allow_ctrlc = TRUE;
        *disk_mode = BF_MODE_RW;
    } else if (strcmp(pi_version, "latest")) return fail("--version is only valid for update");
    if (!image) {
#ifdef _WIN32
        char *environment_image = kv_win32_getenv_utf8("K_VM_IMAGE");
        image = environment_image;
#else
        image = getenv("K_VM_IMAGE");
#endif
        if (!image) {
#ifdef _WIN32
            char *data = kv_win32_getenv_utf8("XDG_DATA_HOME"), *home = kv_win32_getenv_utf8("HOME");
            if (!home) home = kv_win32_getenv_utf8("USERPROFILE");
#else
            const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
#endif
            if (data && *data) { if (asprintf(&default_image, "%s/k-vm/images/%s", data, "pi-riscv64") < 0) abort(); }
            else if (home) { if (asprintf(&default_image, "%s/.local/share/k-vm/images/%s", home, "pi-riscv64") < 0) abort(); }
            image = default_image;
#ifdef _WIN32
            free(data); free(home);
#endif
        }
#ifdef _WIN32
        if (environment_image) {
            default_image = environment_image;
            image = default_image;
        }
#endif
    }
    if (!local_path(image)) return fail(update ? "supply --image or prepare the default RISC-V Pi image" : "supply --image or prepare a default local image");
    if (strcmp(agent, "shell") && strcmp(agent, "pi"))
        return fail("agent must be shell or pi");
    if (!auto_workspace && (config_dir || strcmp(agent, "shell") || optind != argc))
        return fail("agent options require automatic workspace startup");
    if (strcmp(network, "user") && strcmp(network, "none") && strncmp(network, "tap:", 4))
        return fail("network must be user, none or tap:IFNAME");
#ifdef _WIN32
    if (!strncmp(network, "tap:", 4)) return fail("Windows supports user or none networking; TAP is Linux-only");
#endif
    if (!strncmp(network, "tap:", 4) && (!network[4] || strlen(network + 4) >= 16 || strchr(network + 4, '/')))
        return fail("invalid TAP interface name");
    char *cfg = realpath(image, NULL);
    if (!cfg) { fail("%s: %s (use --image or prepare a default image)", image, strerror(errno)); free(default_image); return -1; }
    free(default_image);
#ifdef _WIN32
    wchar_t *wide_cfg = kv_win32_utf16(cfg);
    DWORD attributes = wide_cfg ? GetFileAttributesW(wide_cfg) : INVALID_FILE_ATTRIBUTES;
    free(wide_cfg);
    if (attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_DIRECTORY) {
#else
    struct stat st;
    if (!stat(cfg, &st) && S_ISDIR(st.st_mode)) {
#endif
        char *file;
        if (asprintf(&file, "%s/vm.cfg", cfg) < 0) abort();
        free(cfg); cfg = file;
    }
    uint8_t *data;
    int len;
    if (load_regular(cfg, 1024 * 1024, &data, &len)) { free(cfg); return -1; }
    virt_machine_set_defaults(p);
    p->cfg_filename = cfg;
    int result = virt_machine_parse_config(p, (char *)data, len);
    free(data);
    if (result < 0) return fail("invalid local image configuration");
    if (memory) p->ram_size = (uint64_t)memory << 20;
    if (p->ram_size < ((uint64_t)MIN_MEMORY_MB << 20) || p->ram_size > ((uint64_t)MAX_MEMORY_MB << 20))
        return fail("image memory must be %d..%d MB", MIN_MEMORY_MB, MAX_MEMORY_MB);
    if (p->display_device) return fail("sandbox images must use the terminal frontend");
    if (cmdline) vm_add_cmdline(p, cmdline);
    BOOL riscv = !strncmp(p->machine_name, "riscv", 5);
    if (!p->files[riscv ? VM_FILE_BIOS : VM_FILE_KERNEL].filename)
        return fail("image requires a local %s", riscv ? "BIOS" : "Linux kernel");

    /* Validate every resource before loading boot files or initializing network. */
    for (int i = 0; i < VM_FILE_COUNT; ++i) {
        VMFileEntry *entry = &p->files[i];
        if (!entry->filename) continue;
        char *path = resolve_resource(cfg, entry->filename);
        if (!path) return -1;
        free(entry->filename); entry->filename = path;
    }
    for (int i = 0; i < p->drive_count; ++i) {
        VMDriveEntry *entry = &p->tab_drive[i];
        char *path = resolve_resource(cfg, entry->filename);
        if (!path) return -1;
        free(entry->filename); entry->filename = path;
        if (check_raw_disk(path)) return -1;
    }
    for (int i = 0; i < p->fs_count; ++i) {
        VMFSEntry *entry = &p->tab_fs[i];
        char *path = resolve_resource(cfg, entry->filename);
        if (!path) return -1;
        free(entry->filename); entry->filename = path;
        entry->confined = TRUE;
        entry->read_only = TRUE;
        /* Configured root filesystem tags may retain upstream /dev/root syntax. */
        for (int j = 0; j < i; ++j)
            if (!strcmp(entry->tag, p->tab_fs[j].tag)) return fail("duplicate image filesystem tag");
    }
    for (int i = 0; i < count; ++i) if (add_mapping(p, maps[i])) return -1;
    for (int i = 0; i < VM_FILE_COUNT; ++i) {
        VMFileEntry *entry = &p->files[i];
        if (entry->filename && load_regular(entry->filename, p->ram_size / 2,
                                           &entry->buf, &entry->len)) return -1;
    }
    uint64_t boot_bytes = (uint64_t)p->files[VM_FILE_BIOS].len + p->files[VM_FILE_KERNEL].len + (8 << 20);
    if (boot_bytes > p->ram_size) return fail("boot files exceed available RAM");
    if (riscv) {
        uint64_t bios_end = p->files[VM_FILE_BIOS].len;
        uint64_t align = !strcmp(p->machine_name, "riscv32") ? (4 << 20) : (2 << 20);
        uint64_t kernel_end = p->files[VM_FILE_KERNEL].len ?
            ((bios_end + align - 1) & ~(align - 1)) + p->files[VM_FILE_KERNEL].len : bios_end;
        uint64_t initrd_base = p->ram_size / 2;
        if (initrd_base > (128 << 20)) initrd_base = 128 << 20;
        if (kernel_end > p->ram_size || (p->files[VM_FILE_INITRD].len &&
            (kernel_end > initrd_base || (uint64_t)p->files[VM_FILE_INITRD].len > p->ram_size - initrd_base)))
            return fail("local BIOS/kernel/initrd placements overlap or exceed RAM");
    }
    for (int i = 0; i < p->eth_count; ++i) {
        free(p->tab_eth[i].driver); free(p->tab_eth[i].ifname);
        memset(&p->tab_eth[i], 0, sizeof(p->tab_eth[i]));
    }
    p->eth_count = strcmp(network, "none") ? 1 : 0;
    if (p->eth_count) {
        p->tab_eth[0].driver = strdup(!strncmp(network, "tap:", 4) ? "tap" : "user");
        if (!strncmp(network, "tap:", 4)) p->tab_eth[0].ifname = strdup(network + 4);
    }
    if (auto_workspace) {
        if (check_image_capabilities(cfg, agent)) return -1;
        if (cmdline && (strstr(cmdline, "init=") || strstr(cmdline, "rdinit=")))
            return fail("init overrides require --no-workspace");
        if (update) {
            if (p->drive_count != 1 || p->fs_count) return fail("update requires one root disk and no image filesystem exports");
            if (workspace_prepare_update(p, pi_version)) return fail("invalid update version or launch setup failure");
        } else if (workspace_prepare(p, agent, config_dir, argc - optind, (const char *const *)(argv + optind)))
            return -1;
        vm_add_cmdline(p, "init=/sbin/k-vm-init");
    }
    return 0;
}
