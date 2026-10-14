/* macOS/POSIX descriptor-relative IO. SPDX-License-Identifier: MIT
 * Darwin resolves each parent path in one O_NOFOLLOW_ANY lookup. The portable
 * test implementation uses no-follow directory descriptors component by component.
 * Final opens cannot pin/reopen a vnode like Linux O_PATH: verify identity and
 * defer truncation until validation. See README for the special-file race limit.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "host_io.h"

static int valid_path(const char *path)
{
    if (*path == '/') { errno = EPERM; return 0; }
    const char *start = path;
    for (const char *p = path;; ++p) {
        if (*p == '/' || !*p) {
            size_t len = p - start;
            if (len == 2 && !memcmp(start, "..", 2)) { errno = EPERM; return 0; }
            if (!*p) break;
            start = p + 1;
        }
    }
    return 1;
}

int host_open_directory(int root, const char *path)
{
    if (!valid_path(path)) return -1;
#ifdef __APPLE__
    /* Darwin rejects combining O_NOFOLLOW_ANY with O_NOFOLLOW (EINVAL).
     * O_NOFOLLOW_ANY already rejects symlinks in every path component. */
    return openat(root, *path ? path : ".", O_RDONLY | O_DIRECTORY |
                  O_NOFOLLOW_ANY | O_CLOEXEC);
#else
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *copy = strdup(path), *save = NULL;
    if (!copy) { close(fd); return -1; }
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        close(fd);
        fd = next;
        if (fd < 0) { errno = error; break; }
    }
    free(copy);
    return fd;
#endif
}

static int parent(int root, const char *path, char **name)
{
    if (!valid_path(path)) return -1;
    char *copy = strdup(*path ? path : ".");
    if (!copy) return -1;
    char *slash = strrchr(copy, '/');
    const char *directory = "";
    if (slash) { *slash = 0; directory = copy; }
    *name = strdup(slash ? slash + 1 : copy);
    if (!*name) { free(copy); return -1; }
    int fd = host_open_directory(root, directory), error = errno;
    free(copy);
    if (fd < 0) { free(*name); *name = NULL; }
    errno = error;
    return fd;
}

int host_lstat(int root, const char *path, struct stat *st)
{
    char *name;
    int fd = parent(root, path, &name);
    if (fd < 0) return -1;
    int result = fstatat(fd, name, st, AT_SYMLINK_NOFOLLOW), error = errno;
    close(fd); free(name); errno = error;
    return result;
}

int host_open_file(int root, const char *path, int flags)
{
    char *name;
    int dir = parent(root, path, &name);
    if (dir < 0) return -1;
    struct stat before, after;
    int fd = -1;
    if (fstatat(dir, name, &before, AT_SYMLINK_NOFOLLOW)) goto done;
    if ((!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode)) ||
        (S_ISDIR(before.st_mode) && (flags & (O_ACCMODE | O_TRUNC)))) {
        errno = EPERM; goto done;
    }
    fd = openat(dir, name, (flags & ~O_TRUNC) | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) goto done;
    if (fstat(fd, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        (!S_ISREG(after.st_mode) && !S_ISDIR(after.st_mode))) {
        close(fd); fd = -1; errno = EPERM; goto done;
    }
    if ((flags & O_TRUNC) && ftruncate(fd, 0)) {
        int error = errno; close(fd); fd = -1; errno = error;
    }
done:
    { int error = errno; close(dir); free(name); errno = error; }
    return fd;
}

int host_readlink(int root, const char *path, char *buf, size_t size)
{
    char *name;
    int fd = parent(root, path, &name);
    if (fd < 0) return -1;
    int result = readlinkat(fd, name, buf, size), error = errno;
    close(fd); free(name); errno = error;
    return result;
}
