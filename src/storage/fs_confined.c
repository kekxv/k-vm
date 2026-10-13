/*
 * k-vm host mappings. Paths resolve relative to a pinned directory descriptor.
 * Host descriptor operations are selected for Linux or macOS at build time.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "host_stat.h"
#include "host_io.h"
#include "cutils.h"
#include "fs.h"
#include "host_lock.h"

typedef struct {
    FSDevice common;
    int root_fd;
    BOOL read_only;
    char *single_name;
} ConfinedFS;

struct FSFile {
    char *path; /* relative to root_fd; empty means export root */
    int fd;
    DIR *dir;
};

static int p9_error(void)
{
    switch (errno) {
    case EACCES: case EPERM: case EROFS: case EXDEV: case ELOOP: return -P9_EPERM;
    case ENOENT: return -P9_ENOENT;
    case ENOTDIR: return -P9_ENOTDIR;
    case EEXIST: return -P9_EEXIST;
    case ENOSPC: return -P9_ENOSPC;
    case ENOTEMPTY: return -P9_ENOTEMPTY;
    case ENOSYS: case EOPNOTSUPP: return -P9_ENOTSUP;
    case EIO: return -P9_EIO;
    default: return -P9_EINVAL;
    }
}

static void qid_from_stat(FSQID *qid, const struct stat *st)
{
    qid->type = S_ISDIR(st->st_mode) ? P9_QTDIR : S_ISLNK(st->st_mode) ? P9_QTSYMLINK : P9_QTFILE;
    qid->version = 0;
    qid->path = st->st_ino;
}

static int allowed_path(ConfinedFS *fs, const char *path)
{
    if (fs->single_name && *path && strcmp(path, fs->single_name)) {
        errno = EPERM; return 0;
    }
    return 1;
}

static int open_checked(ConfinedFS *fs, const char *path, int flags)
{
    return allowed_path(fs, path) ? host_open_file(fs->root_fd, path, flags) : -1;
}

static int path_stat(ConfinedFS *fs, const char *path, struct stat *st)
{
    if (!allowed_path(fs, path)) return -1;
    int ret = host_lstat(fs->root_fd, path, st);
    if (!ret && fs->single_name && !*path) {
        st->st_mode = S_IFDIR | 0555;
        st->st_size = 0;
    }
    return ret;
}

static FSFile *new_fid(const char *path)
{
    FSFile *f = mallocz(sizeof(*f));
    f->path = strdup(path);
    f->fd = -1;
    return f;
}

static void close_fid(FSDevice *fs, FSFile *f)
{
    (void)fs;
    if (f->dir) {
        closedir(f->dir);
        f->dir = NULL;
    } else if (f->fd >= 0) {
        close(f->fd);
    }
    f->fd = -1;
}

static void delete_fid(FSDevice *fs, FSFile *f)
{
    if (!f)
        return;
    close_fid(fs, f);
    free(f->path);
    free(f);
}

static BOOL valid_name(const char *name)
{
    return name && *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static char *child_path(const char *parent, const char *name)
{
    char *path;
    if (asprintf(&path, "%s%s%s", parent, *parent ? "/" : "", name) < 0)
        abort();
    return path;
}

static int attach(FSDevice *s, FSFile **out, FSQID *qid, uint32_t uid,
                  const char *uname, const char *aname)
{
    (void)uid; (void)uname; (void)aname;
    struct stat st;
    *out = NULL;
    if (path_stat((ConfinedFS *)s, "", &st))
        return p9_error();
    *out = new_fid("");
    qid_from_stat(qid, &st);
    return 0;
}

static int walk(FSDevice *s, FSFile **out, FSQID *qids, FSFile *f, int n, char **names)
{
    ConfinedFS *fs = (ConfinedFS *)s;
    FSFile *next = new_fid(f->path);
    int i;
    for (i = 0; i < n; ++i) {
        struct stat st;
        if (path_stat(fs, next->path, &st) || !S_ISDIR(st.st_mode))
            break;
        char *path;
        if (!strcmp(names[i], ".")) {
            path = strdup(next->path);
        } else if (!strcmp(names[i], "..")) {
            path = strdup(next->path);
            char *slash = strrchr(path, '/');
            if (slash) *slash = '\0'; else *path = '\0';
        } else if (valid_name(names[i])) {
            path = child_path(next->path, names[i]);
        } else {
            break;
        }
        if (path_stat(fs, path, &st)) {
            free(path);
            break;
        }
        free(next->path);
        next->path = path;
        qid_from_stat(&qids[i], &st);
    }
    *out = next;
    return i;
}

static int host_flags(uint32_t flags)
{
    int ret = flags & P9_O_NOACCESS;
    if (flags & P9_O_TRUNC) ret |= O_TRUNC;
    if (flags & P9_O_EXCL) ret |= O_EXCL;
    if (flags & P9_O_APPEND) ret |= O_APPEND;
    if (flags & P9_O_DSYNC) ret |= O_DSYNC;
    if (flags & P9_O_SYNC) ret |= O_SYNC;
    if (flags & P9_O_DIRECTORY) ret |= O_DIRECTORY;
    /* Do not block on FIFOs or accidentally open host device nodes. */
    return ret | O_NONBLOCK | O_NOFOLLOW;
}

static int open_fid(FSDevice *s, FSQID *qid, FSFile *f, uint32_t flags,
                    FSOpenCompletionFunc *cb, void *opaque)
{
    (void)cb; (void)opaque;
    ConfinedFS *fs = (ConfinedFS *)s;
    struct stat st;
    if (fs->read_only && (flags & (P9_O_NOACCESS | P9_O_TRUNC | P9_O_CREAT | P9_O_APPEND)))
        return -P9_EPERM;
    if (path_stat(fs, f->path, &st))
        return p9_error();
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
        return -P9_EPERM;
    if (S_ISDIR(st.st_mode) && (flags & (P9_O_NOACCESS | P9_O_TRUNC)))
        return -P9_EPERM;
    close_fid(s, f);
    int fd = open_checked(fs, f->path, host_flags(flags));
    if (fd < 0)
        return p9_error();
    if (fstat(fd, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
        close(fd);
        return -P9_EPERM;
    }
    qid_from_stat(qid, &st);
    if (S_ISDIR(st.st_mode)) {
        f->dir = fdopendir(fd);
        if (!f->dir) { close(fd); return p9_error(); }
    } else {
        f->fd = fd;
    }
    return 0;
}

static int parent_fd(ConfinedFS *fs, FSFile *f, const char *name)
{
    if (fs->read_only || fs->single_name || !valid_name(name)) {
        errno = EPERM;
        return -1;
    }
    return allowed_path(fs, f->path) ? host_open_directory(fs->root_fd, f->path) : -1;
}

static int create(FSDevice *s, FSQID *qid, FSFile *f, const char *name,
                  uint32_t flags, uint32_t mode, uint32_t gid)
{
    (void)gid;
    ConfinedFS *fs = (ConfinedFS *)s;
    int parent = parent_fd(fs, f, name);
    if (parent < 0) return p9_error();
    /* Final component never follows a symlink; parent descriptor is confined. */
    int fd = openat(parent, name, host_flags(flags) | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
    if (fd < 0 && errno == EEXIST && !(flags & P9_O_EXCL)) {
        fd = host_open_file(parent, name, host_flags(flags) & ~O_EXCL);
    }
    int saved_errno = errno;
    close(parent);
    errno = saved_errno;
    if (fd < 0) return p9_error();
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -P9_EPERM; }
    char *path = child_path(f->path, name);
    close_fid(s, f);
    free(f->path);
    f->path = path;
    f->fd = fd;
    qid_from_stat(qid, &st);
    return 0;
}

static int mkdir_fid(FSDevice *s, FSQID *qid, FSFile *f, const char *name,
                     uint32_t mode, uint32_t gid)
{
    (void)gid;
    int parent = parent_fd((ConfinedFS *)s, f, name);
    if (parent < 0) return p9_error();
    int ret = mkdirat(parent, name, mode & 0777);
    struct stat st;
    if (!ret) ret = fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW);
    int result = ret ? p9_error() : 0;
    if (!ret) qid_from_stat(qid, &st);
    close(parent);
    return result;
}

static int read_fid(FSDevice *s, FSFile *f, uint64_t offset, uint8_t *buf, int count)
{
    (void)s;
    if (f->fd < 0 || count < 0 || offset > INT64_MAX) return -P9_EPROTO;
    ssize_t n = pread(f->fd, buf, count, offset);
    return n < 0 ? p9_error() : (int)n;
}

static int write_fid(FSDevice *s, FSFile *f, uint64_t offset, const uint8_t *buf, int count)
{
    if (((ConfinedFS *)s)->read_only) return -P9_EPERM;
    if (f->fd < 0 || count < 0 || offset > INT64_MAX) return -P9_EPROTO;
    ssize_t n = pwrite(f->fd, buf, count, offset);
    return n < 0 ? p9_error() : (int)n;
}

static int encode_entry(uint8_t *buf, int count, const char *name, const struct stat *st, uint64_t offset)
{
    size_t len = strlen(name);
    if (len > UINT16_MAX || count < 0 || (size_t)count < 24 + len) return 0;
    FSQID qid;
    qid_from_stat(&qid, st);
    buf[0] = qid.type;
    put_le32(buf + 1, 0);
    put_le64(buf + 5, qid.path);
    put_le64(buf + 13, offset);
    buf[21] = st->st_mode >> 12;
    put_le16(buf + 22, len);
    memcpy(buf + 24, name, len);
    return 24 + len;
}

static int readdir_fid(FSDevice *s, FSFile *f, uint64_t offset, uint8_t *buf, int count)
{
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!f->dir || count < 0) return -P9_EPROTO;
    if (fs->single_name && !*f->path) {
        if (offset) return 0;
        struct stat st;
        if (path_stat(fs, fs->single_name, &st)) return p9_error();
        return encode_entry(buf, count, fs->single_name, &st, 1);
    }
    if (offset > LONG_MAX) return -P9_EINVAL;
    if (!offset) rewinddir(f->dir); else seekdir(f->dir, offset);
    int pos = 0;
    struct dirent *de;
    while ((de = readdir(f->dir))) {
        struct stat st;
        if (fstatat(dirfd(f->dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int n = encode_entry(buf + pos, count - pos, de->d_name, &st, telldir(f->dir));
        if (!n) break;
        pos += n;
    }
    return pos;
}

static int stat_fid(FSDevice *s, FSFile *f, FSStat *out)
{
    struct stat st;
    if (path_stat((ConfinedFS *)s, f->path, &st)) return p9_error();
    memset(out, 0, sizeof(*out));
    qid_from_stat(&out->qid, &st);
    out->st_mode = st.st_mode;
    out->st_uid = st.st_uid; out->st_gid = st.st_gid;
    out->st_nlink = st.st_nlink; out->st_rdev = st.st_rdev;
    out->st_size = st.st_size; out->st_blksize = st.st_blksize; out->st_blocks = st.st_blocks;
    out->st_atime_sec = HOST_STAT_ATIME(st).tv_sec; out->st_atime_nsec = HOST_STAT_ATIME(st).tv_nsec;
    out->st_mtime_sec = HOST_STAT_MTIME(st).tv_sec; out->st_mtime_nsec = HOST_STAT_MTIME(st).tv_nsec;
    out->st_ctime_sec = HOST_STAT_CTIME(st).tv_sec; out->st_ctime_nsec = HOST_STAT_CTIME(st).tv_nsec;
    return 0;
}

static void statfs_fid(FSDevice *s, FSStatFS *out)
{
    struct statfs st;
    memset(out, 0, sizeof(*out));
    if (fstatfs(((ConfinedFS *)s)->root_fd, &st)) return;
    out->f_bsize = st.f_bsize; out->f_blocks = st.f_blocks;
    out->f_bfree = st.f_bfree; out->f_bavail = st.f_bavail;
    out->f_files = st.f_files; out->f_ffree = st.f_ffree;
}

static int setattr_fid(FSDevice *s, FSFile *f, uint32_t mask, uint32_t mode,
                       uint32_t uid, uint32_t gid, uint64_t size, uint64_t atime_sec,
                       uint64_t atime_nsec, uint64_t mtime_sec, uint64_t mtime_nsec)
{
    (void)uid; (void)gid;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (fs->read_only || (fs->single_name && !*f->path)) return -P9_EPERM;
    /* Guest ownership changes and host device nodes are intentionally unsupported. */
    if (mask & (P9_SETATTR_UID | P9_SETATTR_GID)) return -P9_EPERM;
    if (size > INT64_MAX || atime_nsec >= 1000000000 || mtime_nsec >= 1000000000) return -P9_EINVAL;
    int fd = open_checked(fs, f->path, (mask & P9_SETATTR_SIZE ? O_WRONLY : O_RDONLY) | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return p9_error();
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) { close(fd); return -P9_EPERM; }
    if (mask & P9_SETATTR_MODE) ret = fchmod(fd, mode & 0777);
    if (!ret && mask & P9_SETATTR_SIZE) ret = ftruncate(fd, size);
    if (!ret && mask & (P9_SETATTR_ATIME | P9_SETATTR_MTIME)) {
        struct timespec ts[2] = {
            {(time_t)atime_sec, mask & P9_SETATTR_ATIME ? (mask & P9_SETATTR_ATIME_SET ? (long)atime_nsec : UTIME_NOW) : UTIME_OMIT},
            {(time_t)mtime_sec, mask & P9_SETATTR_MTIME ? (mask & P9_SETATTR_MTIME_SET ? (long)mtime_nsec : UTIME_NOW) : UTIME_OMIT},
        };
        ret = futimens(fd, ts);
    }
    int result = ret ? p9_error() : 0;
    close(fd);
    return result;
}

static int rename_fid(FSDevice *s, FSFile *f, const char *name, FSFile *dest, const char *new_name)
{
    ConfinedFS *fs = (ConfinedFS *)s;
    int from = parent_fd(fs, f, name);
    if (from < 0) return p9_error();
    int to = parent_fd(fs, dest, new_name);
    if (to < 0) { close(from); return p9_error(); }
    int ret = renameat(from, name, to, new_name);
    int result = ret ? p9_error() : 0;
    close(from); close(to);
    return result;
}

static int unlink_fid(FSDevice *s, FSFile *f, const char *name)
{
    int parent = parent_fd((ConfinedFS *)s, f, name);
    if (parent < 0) return p9_error();
    int ret = unlinkat(parent, name, 0);
    if (ret && (errno == EISDIR || errno == EPERM)) ret = unlinkat(parent, name, AT_REMOVEDIR);
    int result = ret ? p9_error() : 0;
    close(parent);
    return result;
}

static int link_fid(FSDevice *s, FSFile *dest, FSFile *f, const char *name)
{
    (void)s; (void)dest; (void)f; (void)name;
    return -P9_EPERM; /* Do not create hard links across mapping boundaries. */
}

static int symlink_fid(FSDevice *s, FSQID *qid, FSFile *f, const char *name, const char *target, uint32_t gid)
{
    (void)s; (void)qid; (void)f; (void)name; (void)target; (void)gid;
    return -P9_EPERM; /* Symlink traversal is disabled for sandbox host mappings. */
}

static int mknod_fid(FSDevice *s, FSQID *qid, FSFile *f, const char *name, uint32_t mode, uint32_t major, uint32_t minor, uint32_t gid)
{
    (void)s; (void)qid; (void)f; (void)name; (void)mode; (void)major; (void)minor; (void)gid;
    return -P9_EPERM;
}

static int readlink_fid(FSDevice *s, char *buf, int size, FSFile *f)
{
    if (size <= 0) return -P9_EINVAL;
    ConfinedFS *fs = (ConfinedFS *)s;
    if (!allowed_path(fs, f->path)) return p9_error();
    ssize_t n = host_readlink(fs->root_fd, f->path, buf, size - 1);
    int result = n < 0 ? p9_error() : 0;
    if (n >= 0) buf[n] = '\0';
    return result;
}

static int lock_fid(FSDevice *s, FSFile *f, const FSLock *lock)
{
    (void)s;
    if (f->fd < 0) return -P9_EPROTO;
    struct flock fl = {.l_type = host_lock_type(lock->type), .l_whence = SEEK_SET, .l_start = lock->start, .l_len = lock->length};
    if (!fcntl(f->fd, F_SETLK, &fl)) return P9_LOCK_SUCCESS;
    return errno == EAGAIN || errno == EACCES ? P9_LOCK_BLOCKED : p9_error();
}

static int getlock_fid(FSDevice *s, FSFile *f, FSLock *lock)
{
    (void)s;
    if (f->fd < 0) return -P9_EPROTO;
    struct flock fl = {.l_type = host_lock_type(lock->type), .l_whence = SEEK_SET, .l_start = lock->start, .l_len = lock->length};
    if (fcntl(f->fd, F_GETLK, &fl)) return p9_error();
    lock->type = p9_lock_type(fl.l_type); lock->start = fl.l_start; lock->length = fl.l_len;
    return 0;
}

static void end_fs(FSDevice *s)
{
    ConfinedFS *fs = (ConfinedFS *)s;
    close(fs->root_fd);
    free(fs->single_name);
}

FSDevice *fs_confined_init(const char *host_path, BOOL read_only)
{
    char *resolved = realpath(host_path, NULL);
    if (!resolved) return NULL;
    struct stat st;
    if (lstat(resolved, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
        free(resolved); errno = EINVAL; return NULL;
    }
    ConfinedFS *fs = mallocz(sizeof(*fs));
    fs->read_only = read_only;
    if (S_ISREG(st.st_mode)) {
        char *slash = strrchr(resolved, '/');
        fs->single_name = strdup(slash + 1);
        if (slash == resolved) slash[1] = '\0'; else *slash = '\0';
    }
    fs->root_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    free(resolved);
    if (fs->root_fd < 0) { free(fs->single_name); free(fs); return NULL; }
    /* Probe the selected backend before exporting any files. */
    int probe = host_open_directory(fs->root_fd, "");
    if (probe < 0) { int err = errno; end_fs((FSDevice *)fs); free(fs); errno = err; return NULL; }
    close(probe);
    fs->common = (FSDevice){
        .fs_end = end_fs, .fs_delete = delete_fid, .fs_statfs = statfs_fid,
        .fs_attach = attach, .fs_walk = walk, .fs_mkdir = mkdir_fid,
        .fs_open = open_fid, .fs_create = create, .fs_stat = stat_fid,
        .fs_setattr = setattr_fid, .fs_close = close_fid, .fs_readdir = readdir_fid,
        .fs_read = read_fid, .fs_write = write_fid, .fs_link = link_fid,
        .fs_symlink = symlink_fid, .fs_mknod = mknod_fid, .fs_readlink = readlink_fid,
        .fs_renameat = rename_fid, .fs_unlinkat = unlink_fid,
        .fs_lock = lock_fid, .fs_getlock = getlock_fid,
    };
    return (FSDevice *)fs;
}
