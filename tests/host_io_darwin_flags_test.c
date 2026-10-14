/* Exercise Darwin's flag contract on Linux, where CI can check it cheaply. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "src/storage/host_io.h"

static int calls;
int __wrap_openat(int root, const char *path, int flags, ...)
{
    assert(root == 42 && !strcmp(path, "."));
    calls++;
    /* This is the rejection enforced by Darwin vn_open_auth(). */
    if ((flags & O_NOFOLLOW_ANY) && (flags & O_NOFOLLOW)) {
        errno = EINVAL;
        return -1;
    }
    assert(flags & O_NOFOLLOW_ANY);
    assert(flags & O_DIRECTORY);
    return 123;
}

/* glibc redirects openat to openat64 with _FILE_OFFSET_BITS=64. */
int __wrap_openat64(int root, const char *path, int flags, ...)
    __attribute__((alias("__wrap_openat")));

int main(void)
{
    assert(host_open_directory(42, "") == 123);
    assert(calls == 1);
    return 0;
}
