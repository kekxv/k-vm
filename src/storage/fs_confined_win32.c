/* Native Win32 9P directory and single-file exports. SPDX-License-Identifier:
 * MIT */
#include "cutils.h"
#include "fs.h"
#include "win32_files.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct ModeEntry {
    uint64_t id;
    DWORD volume;
    uint32_t mode;
    struct ModeEntry *next;
} ModeEntry;
typedef struct {
    FSDevice common;
    HANDLE root;
    BOOL read_only;
    char *single;
    FSFile *fids;
    ModeEntry *modes;
} ConfinedFS;
struct FSFile {
    char *path;
    FSFile *next;
    KVWin32File open;
    BOOL directory;
    BOOL append;
};
static int p9_error(void) {
    switch (errno) {
    case EACCES:
    case EPERM:
        return -P9_EPERM;
    case ENOENT:
        return -P9_ENOENT;
    case EEXIST:
        return -P9_EEXIST;
    case ENOTDIR:
        return -P9_ENOTDIR;
    case ENOSPC:
        return -P9_ENOSPC;
    case ENOTEMPTY:
        return -P9_ENOTEMPTY;
    case ENOSYS:
        return -P9_ENOTSUP;
    case EIO:
        return -P9_EIO;
    default:
        return -P9_EINVAL;
    }
}
static int win_error(void) {
    kv_win32_error(GetLastError());
    return p9_error();
}
static FSFile *new_fid(ConfinedFS *fs, const char *path) {
    FSFile *f = calloc(1, sizeof(*f));
    if (!f)
        abort();
    f->path = strdup(path);
    if (!f->path)
        abort();
    f->open.handle = INVALID_HANDLE_VALUE;
    f->next = fs->fids;
    fs->fids = f;
    return f;
}
static void close_fid(FSDevice *s, FSFile *f) {
    (void)s;
    kv_win32_file_close(&f->open);
}
static void delete_fid(FSDevice *s, FSFile *f) {
    if (!f)
        return;
    ConfinedFS *fs = (ConfinedFS *)s;
    FSFile **slot = &fs->fids;
    while (*slot && *slot != f)
        slot = &(*slot)->next;
    if (*slot)
        *slot = f->next;
    close_fid(s, f);
    free(f->path);
    free(f);
}
static char *child(const char *path, const char *name) {
    size_t a = strlen(path), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (!p)
        abort();
    memcpy(p, path, a);
    if (a)
        p[a++] = '/';
    memcpy(p + a, name, b + 1);
    return p;
}
static int resolve(ConfinedFS *fs, const char *path, DWORD access, DWORD share,
                   DWORD disposition, int dir, KVWin32File *o) {
    if (fs->single && *path && strcmp(path, fs->single)) {
        errno = EACCES;
        return -1;
    }
    return kv_win32_relative(fs->root, path, access, share, disposition, dir,
                             o);
}
static void qid_info(FSQID *q, const BY_HANDLE_FILE_INFORMATION *i) {
    q->type =
        i->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? P9_QTDIR : P9_QTFILE;
    q->version = 0;
    q->path = ((uint64_t)i->nFileIndexHigh << 32) | i->nFileIndexLow;
}
static ModeEntry *mode_entry(ConfinedFS *fs,
                             const BY_HANDLE_FILE_INFORMATION *i, int create) {
    uint64_t id = ((uint64_t)i->nFileIndexHigh << 32) | i->nFileIndexLow;
    ModeEntry *entry = fs->modes;
    while (entry &&
           (entry->id != id || entry->volume != i->dwVolumeSerialNumber))
        entry = entry->next;
    if (!entry && create) {
        entry = calloc(1, sizeof(*entry));
        if (!entry)
            abort();
        entry->id = id;
        entry->volume = i->dwVolumeSerialNumber;
        entry->next = fs->modes;
        fs->modes = entry;
    }
    return entry;
}
static void remember_mode(ConfinedFS *fs, const BY_HANDLE_FILE_INFORMATION *i,
                          uint32_t mode) {
    mode_entry(fs, i, 1)->mode = mode & 0777;
}
static int path_info(ConfinedFS *fs, const char *path,
                     BY_HANDLE_FILE_INFORMATION *i) {
    KVWin32File o;
    if (resolve(fs, path, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                OPEN_EXISTING, -1, &o))
        return -1;
    BOOL ok = GetFileInformationByHandle(o.handle, i);
    DWORD e = GetLastError();
    kv_win32_file_close(&o);
    if (!ok)
        return kv_win32_error(e);
    if (fs->single && !*path) {
        i->dwFileAttributes =
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        i->nFileSizeHigh = i->nFileSizeLow = 0;
    }
    return 0;
}
static int attach(FSDevice *s, FSFile **out, FSQID *q, uint32_t uid,
                  const char *u, const char *a) {
    (void)uid;
    (void)u;
    (void)a;
    BY_HANDLE_FILE_INFORMATION i;
    *out = NULL;
    if (path_info((ConfinedFS *)s, "", &i))
        return p9_error();
    *out = new_fid((ConfinedFS *)s, "");
    qid_info(q, &i);
    return 0;
}
static int walk(FSDevice *s, FSFile **out, FSQID *qs, FSFile *f, int n,
                char **names) {
    ConfinedFS *fs = (ConfinedFS *)s;
    FSFile *next = new_fid(fs, f->path);
    int k;
    for (k = 0; k < n; k++) {
        BY_HANDLE_FILE_INFORMATION i;
        if (path_info(fs, next->path, &i) ||
            !(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            break;
        char *p;
        if (!strcmp(names[k], "."))
            p = strdup(next->path);
        else if (!strcmp(names[k], "..")) {
            p = strdup(next->path);
            char *slash = strrchr(p, '/');
            if (slash)
                *slash = 0;
            else
                *p = 0;
        } else if (kv_win32_valid_name(names[k]))
            p = child(next->path, names[k]);
        else
            break;
        if (path_info(fs, p, &i)) {
            free(p);
            break;
        }
        free(next->path);
        next->path = p;
        qid_info(qs + k, &i);
    }
    *out = next;
    return k;
}
static DWORD access_flags(uint32_t f) {
    return (f & P9_O_NOACCESS) == P9_O_WRONLY ? GENERIC_WRITE
           : (f & P9_O_NOACCESS) == P9_O_RDWR ? GENERIC_READ | GENERIC_WRITE
                                              : GENERIC_READ;
}
static int open_fid(FSDevice *s, FSQID *q, FSFile *f, uint32_t flags,
                    FSOpenCompletionFunc *cb, void *opaque) {
    (void)cb;
    (void)opaque;
    ConfinedFS *fs = (ConfinedFS *)s;
    BY_HANDLE_FILE_INFORMATION i;
    if (fs->read_only &&
        (flags & (P9_O_NOACCESS | P9_O_TRUNC | P9_O_CREAT | P9_O_APPEND)))
        return -P9_EPERM;
    if (path_info(fs, f->path, &i))
        return p9_error();
    BOOL dir = !!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (dir && (flags & (P9_O_NOACCESS | P9_O_TRUNC)))
        return -P9_EPERM;
    if ((flags & P9_O_DIRECTORY) && !dir)
        return -P9_ENOTDIR;
    KVWin32File o;
    if (resolve(fs, f->path,
                access_flags(flags) | FILE_READ_ATTRIBUTES |
                    (fs->read_only ? 0 : FILE_WRITE_ATTRIBUTES),
                dir ? FILE_SHARE_READ | FILE_SHARE_WRITE
                    : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                flags & P9_O_TRUNC ? TRUNCATE_EXISTING : OPEN_EXISTING,
                dir ? 1 : 0, &o))
        return p9_error();
    if (!GetFileInformationByHandle(o.handle, &i)) {
        int e = win_error();
        kv_win32_file_close(&o);
        return e;
    }
    close_fid(s, f);
    f->open = o;
    f->directory = dir;
    f->append = !!(flags & P9_O_APPEND);
    qid_info(q, &i);
    return 0;
}
static int writable(ConfinedFS *fs, const char *name) {
    if (fs->read_only || fs->single || !kv_win32_valid_name(name)) {
        errno = EACCES;
        return 0;
    }
    return 1;
}
static int create(FSDevice *s, FSQID *q, FSFile *f, const char *name,
                  uint32_t flags, uint32_t mode, uint32_t gid) {
    (void)gid;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!writable(fs, name))
        return p9_error();
    char *p = child(f->path, name);
    KVWin32File o;
    DWORD access =
        access_flags(flags) | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    BOOL created = TRUE;
    if (resolve(fs, p, access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                CREATE_NEW, 0, &o)) {
        if (errno != EEXIST || (flags & P9_O_EXCL)) {
            free(p);
            return p9_error();
        }
        created = FALSE;
        if (resolve(fs, p, access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    flags & P9_O_TRUNC ? TRUNCATE_EXISTING : OPEN_EXISTING, 0,
                    &o)) {
            free(p);
            return p9_error();
        }
    }
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(o.handle, &i)) {
        int e = win_error();
        kv_win32_file_close(&o);
        free(p);
        return e;
    }
    close_fid(s, f);
    free(f->path);
    f->path = p;
    f->open = o;
    if (created)
        remember_mode(fs, &i, mode);
    f->directory = FALSE;
    f->append = !!(flags & P9_O_APPEND);
    qid_info(q, &i);
    return 0;
}
static int mkdir_fid(FSDevice *s, FSQID *q, FSFile *f, const char *name,
                     uint32_t mode, uint32_t gid) {
    (void)gid;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!writable(fs, name))
        return p9_error();
    char *p = child(f->path, name);
    KVWin32File o;
    int ret = resolve(fs, p, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_NEW, 1, &o);
    free(p);
    if (ret)
        return p9_error();
    BY_HANDLE_FILE_INFORMATION i;
    BOOL ok = GetFileInformationByHandle(o.handle, &i);
    int e = ok ? 0 : win_error();
    if (ok) {
        qid_info(q, &i);
        remember_mode(fs, &i, mode);
    }
    kv_win32_file_close(&o);
    return e;
}
static int io_fid(FSDevice *s, FSFile *f, uint64_t offset, void *buf, int count,
                  int write) {
    if (write && ((ConfinedFS *)s)->read_only)
        return -P9_EPERM;
    if (f->open.handle == INVALID_HANDLE_VALUE || f->directory || count < 0 ||
        offset > INT64_MAX)
        return -P9_EPROTO;
    LARGE_INTEGER pos;
    pos.QuadPart = write && f->append ? 0 : offset;
    if (!SetFilePointerEx(f->open.handle, pos, NULL,
                          write && f->append ? FILE_END : FILE_BEGIN))
        return win_error();
    DWORD n;
    BOOL ok = write ? WriteFile(f->open.handle, buf, count, &n, NULL)
                    : ReadFile(f->open.handle, buf, count, &n, NULL);
    return ok ? (int)n : win_error();
}
static int read_fid(FSDevice *s, FSFile *f, uint64_t offset, uint8_t *buf,
                    int count) {
    return io_fid(s, f, offset, buf, count, 0);
}
static int write_fid(FSDevice *s, FSFile *f, uint64_t offset,
                     const uint8_t *buf, int count) {
    return io_fid(s, f, offset, (void *)buf, count, 1);
}
static void filetime_stat(FILETIME t, uint64_t *sec, uint32_t *nsec) {
    uint64_t v = ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (v < epoch) {
        *sec = 0;
        *nsec = 0;
    } else {
        v -= epoch;
        *sec = v / 10000000;
        *nsec = (v % 10000000) * 100;
    }
}
static int stat_fid(FSDevice *s, FSFile *f, FSStat *o) {
    BY_HANDLE_FILE_INFORMATION i;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (f->open.handle != INVALID_HANDLE_VALUE) {
        if (!GetFileInformationByHandle(f->open.handle, &i))
            return win_error();
    } else if (path_info(fs, f->path, &i))
        return p9_error();
    if (fs->single && !*f->path) {
        i.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        i.nFileSizeHigh = i.nFileSizeLow = 0;
    }
    memset(o, 0, sizeof(*o));
    qid_info(&o->qid, &i);
    ModeEntry *entry = mode_entry(fs, &i, 0);
    uint32_t perms =
        entry ? entry->mode
              : ((i.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0555 : 0777);
    if (fs->read_only || (fs->single && !*f->path))
        perms &= ~0222;
    o->st_mode = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? P9_S_IFDIR
                                                                : P9_S_IFREG) |
                 perms;
    o->st_nlink = i.nNumberOfLinks;
    o->st_size = ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow;
    o->st_blksize = 4096;
    o->st_blocks = (o->st_size + 511) / 512;
    filetime_stat(i.ftLastAccessTime, &o->st_atime_sec, &o->st_atime_nsec);
    filetime_stat(i.ftLastWriteTime, &o->st_mtime_sec, &o->st_mtime_nsec);
    filetime_stat(i.ftCreationTime, &o->st_ctime_sec, &o->st_ctime_nsec);
    return 0;
}
static void statfs_fid(FSDevice *s, FSStatFS *o) {
    memset(o, 0, sizeof(*o));
    wchar_t path[32768];
    if (!GetFinalPathNameByHandleW(((ConfinedFS *)s)->root, path, 32768, 0))
        return;
    ULARGE_INTEGER avail, total, freeb;
    if (!GetDiskFreeSpaceExW(path, &avail, &total, &freeb))
        return;
    o->f_bsize = 4096;
    o->f_blocks = total.QuadPart / 4096;
    o->f_bfree = freeb.QuadPart / 4096;
    o->f_bavail = avail.QuadPart / 4096;
}
static FILETIME time_value(uint64_t sec, uint64_t ns) {
    uint64_t v = sec * 10000000 + ns / 100 + 116444736000000000ULL;
    FILETIME t = {(DWORD)v, (DWORD)(v >> 32)};
    return t;
}
static int setattr_fid(FSDevice *s, FSFile *f, uint32_t mask, uint32_t mode,
                       uint32_t uid, uint32_t gid, uint64_t size, uint64_t as,
                       uint64_t an, uint64_t ms, uint64_t mn) {
    (void)uid;
    (void)gid;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (fs->read_only || (fs->single && !*f->path) ||
        (mask & (P9_SETATTR_UID | P9_SETATTR_GID)))
        return -P9_EPERM;
    /* 9P callers may fill fields excluded by mask with sentinel values. */
    if (((mask & P9_SETATTR_SIZE) && size > INT64_MAX) ||
        ((mask & P9_SETATTR_ATIME) && (mask & P9_SETATTR_ATIME_SET) &&
         (an >= 1000000000 ||
          as > (UINT64_MAX - 116444736000000000ULL - 9999999) / 10000000)) ||
        ((mask & P9_SETATTR_MTIME) && (mask & P9_SETATTR_MTIME_SET) &&
         (mn >= 1000000000 ||
          ms > (UINT64_MAX - 116444736000000000ULL - 9999999) / 10000000)))
        return -P9_EINVAL;
    KVWin32File o = {.handle = INVALID_HANDLE_VALUE};
    DWORD access = FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES |
                   (mask & P9_SETATTR_SIZE ? GENERIC_WRITE : 0);
    if (f->open.handle != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), f->open.handle,
                             GetCurrentProcess(), &o.handle, access, FALSE,
                             0)) {
            DWORD original = GetLastError();
            wchar_t path[32768];
            if (!GetFinalPathNameByHandleW(f->open.handle, path, 32768, 0)) {
                SetLastError(original);
                return win_error();
            }
            char *utf8 = kv_win32_utf8(path);
            if (!utf8)
                return p9_error();
            o.handle = kv_win32_open(utf8, access,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE |
                                         FILE_SHARE_DELETE,
                                     OPEN_EXISTING, 0);
            free(utf8);
            if (o.handle == INVALID_HANDLE_VALUE)
                return p9_error();
            BY_HANDLE_FILE_INFORMATION before, after;
            if (!GetFileInformationByHandle(f->open.handle, &before) ||
                !GetFileInformationByHandle(o.handle, &after) ||
                before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
                before.nFileIndexHigh != after.nFileIndexHigh ||
                before.nFileIndexLow != after.nFileIndexLow) {
                kv_win32_file_close(&o);
                return -P9_EPERM;
            }
        }
    } else if (resolve(fs, f->path, access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       OPEN_EXISTING, -1, &o))
        return p9_error();
    BOOL ok = TRUE;
    if (mask & P9_SETATTR_MODE) {
        FILE_BASIC_INFO info;
        ok = GetFileInformationByHandleEx(o.handle, FileBasicInfo, &info,
                                          sizeof(info));
        if (ok) {
            if (mode & 0222)
                info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            else
                info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
            if (!info.FileAttributes)
                info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            ok = SetFileInformationByHandle(o.handle, FileBasicInfo, &info,
                                            sizeof(info));
        }
    }
    if (ok && (mask & P9_SETATTR_SIZE)) {
        LARGE_INTEGER pos;
        pos.QuadPart = size;
        ok = SetFilePointerEx(o.handle, pos, NULL, FILE_BEGIN) &&
             SetEndOfFile(o.handle);
    }
    if (ok && (mask & (P9_SETATTR_ATIME | P9_SETATTR_MTIME))) {
        FILETIME at = time_value(as, an), mt = time_value(ms, mn), now;
        GetSystemTimeAsFileTime(&now);
        if (!(mask & P9_SETATTR_ATIME_SET))
            at = now;
        if (!(mask & P9_SETATTR_MTIME_SET))
            mt = now;
        ok = SetFileTime(o.handle, NULL, mask & P9_SETATTR_ATIME ? &at : NULL,
                         mask & P9_SETATTR_MTIME ? &mt : NULL);
    }
    int e = ok ? 0 : win_error();
    if (ok && (mask & P9_SETATTR_MODE)) {
        BY_HANDLE_FILE_INFORMATION info;
        if (GetFileInformationByHandle(o.handle, &info))
            remember_mode(fs, &info, mode);
        else
            e = win_error();
    }
    kv_win32_file_close(&o);
    return e;
}
static int encode_entry(uint8_t *buf, int count, const char *name,
                        const BY_HANDLE_FILE_INFORMATION *i, uint64_t off) {
    size_t n = strlen(name);
    if (n > UINT16_MAX || count < 0 || (size_t)count < 24 + n)
        return 0;
    FSQID q;
    qid_info(&q, i);
    buf[0] = q.type;
    put_le32(buf + 1, 0);
    put_le64(buf + 5, q.path);
    put_le64(buf + 13, off);
    buf[21] = q.type == P9_QTDIR ? 4 : 8;
    put_le16(buf + 22, n);
    memcpy(buf + 24, name, n);
    return (int)n + 24;
}
static int readdir_fid(FSDevice *s, FSFile *f, uint64_t off, uint8_t *buf,
                       int count) {
    ConfinedFS *fs = (ConfinedFS *)s;
    if (f->open.handle == INVALID_HANDLE_VALUE || !f->directory || count < 0)
        return -P9_EPROTO;
    if (fs->single && !*f->path) {
        if (off)
            return 0;
        BY_HANDLE_FILE_INFORMATION i;
        if (path_info(fs, fs->single, &i))
            return p9_error();
        return encode_entry(buf, count, fs->single, &i, 1);
    }
    /* Enumerate by the pinned directory handle, never by its mutable pathname.
     */
    unsigned char entries[65536];
    uint64_t index = 0;
    int pos = 0;
    BOOL restart = TRUE;
    for (;;) {
        BOOL ok = GetFileInformationByHandleEx(
            f->open.handle,
            restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
            entries, sizeof(entries));
        restart = FALSE;
        if (!ok) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                return pos;
            return pos ? pos : win_error();
        }
        FILE_ID_BOTH_DIR_INFO *e = (FILE_ID_BOTH_DIR_INFO *)entries;
        for (;;) {
            size_t chars = e->FileNameLength / 2;
            wchar_t *w = malloc((chars + 1) * sizeof(*w));
            if (!w)
                return -P9_EIO;
            memcpy(w, e->FileName, e->FileNameLength);
            w[chars] = 0;
            char *name = kv_win32_utf8(w);
            free(w);
            if (name && kv_win32_valid_name(name) &&
                !(e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                index++;
                if (index > off) {
                    BY_HANDLE_FILE_INFORMATION info;
                    memset(&info, 0, sizeof(info));
                    info.dwFileAttributes = e->FileAttributes;
                    info.nFileIndexLow = e->FileId.LowPart;
                    info.nFileIndexHigh = e->FileId.HighPart;
                    int n = encode_entry(buf + pos, count - pos, name, &info,
                                         index);
                    if (!n) {
                        free(name);
                        return pos;
                    }
                    pos += n;
                }
            }
            free(name);
            if (!e->NextEntryOffset)
                break;
            e = (FILE_ID_BOTH_DIR_INFO *)((char *)e + e->NextEntryOffset);
        }
    }
}
static int rename_fid(FSDevice *s, FSFile *f, const char *name, FSFile *dest,
                      const char *newname) {
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!writable(fs, name) || !kv_win32_valid_name(newname))
        return -P9_EPERM;
    KVWin32File src, to;
    char *p = child(f->path, name);
    int ret = resolve(fs, p, DELETE | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      OPEN_EXISTING, -1, &src);
    free(p);
    if (ret)
        return p9_error();
    if (resolve(fs, dest->path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 1, &to)) {
        ret = p9_error();
        kv_win32_file_close(&src);
        return ret;
    }
    ret =
        kv_win32_rename_handle(src.handle, to.handle, newname) ? p9_error() : 0;
    kv_win32_file_close(&src);
    kv_win32_file_close(&to);
    return ret;
}
static int unlink_fid(FSDevice *s, FSFile *f, const char *name) {
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!writable(fs, name))
        return p9_error();
    char *p = child(f->path, name);
    KVWin32File o;
    if (resolve(fs, p, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                OPEN_EXISTING, -1, &o)) {
        free(p);
        return p9_error();
    }
    free(p);
    FILE_BASIC_INFO oldinfo;
    BOOL have = GetFileInformationByHandleEx(o.handle, FileBasicInfo, &oldinfo,
                                             sizeof(oldinfo));
    BOOL cleared = FALSE;
    if (have && (oldinfo.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
        FILE_BASIC_INFO info = oldinfo;
        info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!info.FileAttributes)
            info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        cleared = SetFileInformationByHandle(o.handle, FileBasicInfo, &info,
                                             sizeof(info));
    }
    FILE_DISPOSITION_INFO d = {TRUE};
    BOOL ok = SetFileInformationByHandle(o.handle, FileDispositionInfo, &d,
                                         sizeof(d));
    int ret = ok ? 0 : win_error();
    if (!ok && cleared)
        SetFileInformationByHandle(o.handle, FileBasicInfo, &oldinfo,
                                   sizeof(oldinfo));
    kv_win32_file_close(&o);
    return ret;
}
static int link_fid(FSDevice *s, FSFile *d, FSFile *f, const char *n) {
    (void)s;
    (void)d;
    (void)f;
    (void)n;
    return -P9_EPERM;
}
static int symlink_fid(FSDevice *s, FSQID *q, FSFile *f, const char *n,
                       const char *t, uint32_t g) {
    (void)s;
    (void)q;
    (void)f;
    (void)n;
    (void)t;
    (void)g;
    return -P9_EPERM;
}
static int mknod_fid(FSDevice *s, FSQID *q, FSFile *f, const char *n,
                     uint32_t m, uint32_t a, uint32_t b, uint32_t g) {
    (void)s;
    (void)q;
    (void)f;
    (void)n;
    (void)m;
    (void)a;
    (void)b;
    (void)g;
    return -P9_EPERM;
}
static int readlink_fid(FSDevice *s, char *b, int n, FSFile *f) {
    (void)s;
    (void)b;
    (void)n;
    (void)f;
    return -P9_EPERM;
}
static int lock_fid(FSDevice *s, FSFile *f, const FSLock *l) {
    (void)s;
    if (f->open.handle == INVALID_HANDLE_VALUE || f->directory)
        return -P9_EPROTO;
    if (l->type > P9_LOCK_TYPE_UNLCK)
        return -P9_EINVAL;
    OVERLAPPED o;
    memset(&o, 0, sizeof(o));
    o.Offset = l->start;
    o.OffsetHigh = l->start >> 32;
    uint64_t len = l->length ? l->length : UINT64_MAX - l->start;
    BOOL ok = l->type == P9_LOCK_TYPE_UNLCK
                  ? UnlockFileEx(f->open.handle, 0, (DWORD)len,
                                 (DWORD)(len >> 32), &o)
                  : LockFileEx(f->open.handle,
                               LOCKFILE_FAIL_IMMEDIATELY |
                                   (l->type == P9_LOCK_TYPE_WRLCK
                                        ? LOCKFILE_EXCLUSIVE_LOCK
                                        : 0),
                               0, (DWORD)len, (DWORD)(len >> 32), &o);
    if (ok)
        return P9_LOCK_SUCCESS;
    return GetLastError() == ERROR_LOCK_VIOLATION ? P9_LOCK_BLOCKED
                                                  : win_error();
}
static int getlock_fid(FSDevice *s, FSFile *f, FSLock *l) {
    (void)s;
    (void)f;
    (void)l;
    return -P9_ENOTSUP; /* Win32 cannot report the conflicting owner's identity.
                         */
}
static void end_fs(FSDevice *s) {
    ConfinedFS *fs = (ConfinedFS *)s;
    while (fs->fids)
        delete_fid(s, fs->fids);
    while (fs->modes) {
        ModeEntry *next = fs->modes->next;
        free(fs->modes);
        fs->modes = next;
    }
    CloseHandle(fs->root);
    fs->root = INVALID_HANDLE_VALUE;
    free(fs->single);
    fs->single = NULL;
}
FSDevice *fs_confined_init(const char *host_path, BOOL read_only) {
    char *path = kv_win32_realpath(host_path);
    if (!path)
        return NULL;
    HANDLE h =
        kv_win32_open(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE) {
        free(path);
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(h, &i)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        free(path);
        kv_win32_error(e);
        return NULL;
    }
    CloseHandle(h);
    ConfinedFS *fs = calloc(1, sizeof(*fs));
    if (!fs) {
        free(path);
        return NULL;
    }
    fs->read_only = read_only;
    if (!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        char *slash = strrchr(path, '\\');
        if (!slash || !kv_win32_valid_name(slash + 1)) {
            free(path);
            free(fs);
            errno = EINVAL;
            return NULL;
        }
        fs->single = strdup(slash + 1);
        slash[1] = 0;
    }
    fs->root =
        kv_win32_open(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    free(path);
    if (fs->root == INVALID_HANDLE_VALUE) {
        free(fs->single);
        free(fs);
        return NULL;
    }
    fs->common = (FSDevice){.fs_end = end_fs,
                            .fs_delete = delete_fid,
                            .fs_statfs = statfs_fid,
                            .fs_attach = attach,
                            .fs_walk = walk,
                            .fs_mkdir = mkdir_fid,
                            .fs_open = open_fid,
                            .fs_create = create,
                            .fs_stat = stat_fid,
                            .fs_setattr = setattr_fid,
                            .fs_close = close_fid,
                            .fs_readdir = readdir_fid,
                            .fs_read = read_fid,
                            .fs_write = write_fid,
                            .fs_link = link_fid,
                            .fs_symlink = symlink_fid,
                            .fs_mknod = mknod_fid,
                            .fs_readlink = readlink_fid,
                            .fs_renameat = rename_fid,
                            .fs_unlinkat = unlink_fid,
                            .fs_lock = lock_fid,
                            .fs_getlock = getlock_fid};
    return (FSDevice *)fs;
}
