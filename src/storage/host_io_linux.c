/* Linux kernel-enforced path resolution. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include "host_io.h"

static int open_path(int root, const char *path, int flags)
{
    struct open_how how = {
        .flags = (unsigned)(flags | O_CLOEXEC),
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,
    };
    return syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof(how));
}

int host_open_directory(int root, const char *path)
{
    return open_path(root, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

int host_open_file(int root, const char *path, int flags)
{
    int pin = open_path(root, path, O_PATH | O_NOFOLLOW);
    if (pin < 0) return -1;
    struct stat st;
    int fd = -1;
    if (!fstat(pin, &st)) {
        if (S_ISDIR(st.st_mode)) {
            if (flags & (O_ACCMODE | O_TRUNC)) errno = EPERM;
            else fd = openat(pin, ".", flags | O_DIRECTORY | O_CLOEXEC);
        } else if (S_ISREG(st.st_mode)) {
            char descriptor[64];
            snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", pin);
            fd = open(descriptor, (flags & ~O_NOFOLLOW) | O_CLOEXEC);
        } else errno = EPERM;
    }
    int error = errno;
    close(pin);
    errno = error;
    return fd;
}

int host_lstat(int root, const char *path, struct stat *st)
{
    int fd = open_path(root, path, O_PATH | O_NOFOLLOW);
    if (fd < 0) return -1;
    int result = fstat(fd, st), error = errno;
    close(fd);
    errno = error;
    return result;
}

int host_readlink(int root, const char *path, char *buf, size_t size)
{
    int fd = open_path(root, path, O_PATH | O_NOFOLLOW);
    if (fd < 0) return -1;
    int result = readlinkat(fd, "", buf, size), error = errno;
    close(fd);
    errno = error;
    return result;
}
