/* Real host files: catch write-policy bypasses and path/symlink escapes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "cutils.h"
#include "fs.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static FSFile *walk(FSDevice *fs, FSFile *root, const char *name, int expected)
{
    FSFile *f = NULL;
    FSQID qid;
    char *names[] = {(char *)name};
    int result = fs->fs_walk(fs, &f, &qid, root, 1, names);
    CHECK(expected ? result == 1 : result <= 0);
    return f;
}

static void put_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    close(fd);
}

int main(void)
{
    char dir[] = "/tmp/k-vm-fs-XXXXXX";
    char root_path[512], file[512], outside[512], linkpath[512], fifo[512];
    FSDevice *fs;
    FSFile *root, *f, *other;
    FSQID qid;
    FSStat st;
    uint8_t buf[128];
    CHECK(mkdtemp(dir));
    snprintf(root_path, sizeof(root_path), "%s/root", dir);
    snprintf(file, sizeof(file), "%s/root/data", dir);
    snprintf(outside, sizeof(outside), "%s/secret", dir);
    snprintf(linkpath, sizeof(linkpath), "%s/root/escape", dir);
    CHECK(mkdir(root_path, 0700) == 0);
    snprintf(fifo, sizeof(fifo), "%s/root/fifo", dir);
    CHECK(mkfifo(fifo, 0600) == 0);
    put_file(file, "mapped");
    put_file(outside, "secret");
    CHECK(symlink(outside, linkpath) == 0);
    fs = fs_confined_init(root_path, TRUE);
    CHECK(fs);
    CHECK(fs->fs_attach(fs, &root, &qid, 0, "", "") == 0);
    f = walk(fs, root, "data", 1);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDONLY, NULL, NULL) == 0);
    CHECK(fs->fs_read(fs, f, 0, buf, sizeof(buf)) == 6);
    CHECK(memcmp(buf, "mapped", 6) == 0);
    CHECK(fs->fs_write(fs, f, 0, (uint8_t *)"bad", 3) < 0);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDWR, NULL, NULL) < 0);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDONLY | P9_O_TRUNC, NULL, NULL) < 0);
    CHECK(fs->fs_setattr(fs, f, P9_SETATTR_SIZE, 0, 0, 0, 0, 0, 0, 0, 0) < 0);
    CHECK(fs->fs_mkdir(fs, &qid, root, "new", 0700, 0) < 0);
    CHECK(fs->fs_create(fs, &qid, root, "new", P9_O_RDWR, 0600, 0) < 0);
    CHECK(fs->fs_unlinkat(fs, root, "data") < 0);
    CHECK(fs->fs_renameat(fs, root, "data", root, "renamed") < 0);
    CHECK(fs->fs_symlink(fs, &qid, root, "new", "data", 0) < 0);
    CHECK(fs->fs_link(fs, root, f, "new") < 0);
    CHECK(fs->fs_mknod(fs, &qid, root, "dev", S_IFCHR | 0600, 1, 3, 0) < 0);
    fs->fs_delete(fs, f);
    f = walk(fs, root, "..", 1);
    CHECK(fs->fs_stat(fs, f, &st) == 0);
    other = walk(fs, f, "secret", 0);
    fs->fs_delete(fs, other);
    fs->fs_delete(fs, f);
    f = walk(fs, root, "../secret", 0);
    fs->fs_delete(fs, f);
    f = walk(fs, root, "escape", 1);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDONLY, NULL, NULL) < 0);
    fs->fs_delete(fs, f);
    fs->fs_delete(fs, root);
    fs_end(fs);

    fs = fs_confined_init(root_path, FALSE);
    CHECK(fs);
    CHECK(fs->fs_attach(fs, &root, &qid, 0, "", "") == 0);
    f = walk(fs, root, "data", 1);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDWR, NULL, NULL) == 0);
    CHECK(fs->fs_write(fs, f, 0, (uint8_t *)"edited", 6) == 6);
    FSLock lock = {.type = P9_LOCK_TYPE_WRLCK, .start = 0, .length = 6};
    CHECK(fs->fs_lock(fs, f, &lock) == P9_LOCK_SUCCESS);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        FSLock query = lock;
        CHECK(fs->fs_getlock(fs, f, &query) == 0);
        CHECK(query.type == P9_LOCK_TYPE_WRLCK);
        CHECK(fs->fs_lock(fs, f, &lock) == P9_LOCK_BLOCKED);
        _exit(0);
    }
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child && child_status == 0);
    lock.type = P9_LOCK_TYPE_UNLCK;
    CHECK(fs->fs_lock(fs, f, &lock) == P9_LOCK_SUCCESS);
    lock.type = P9_LOCK_TYPE_RDLCK;
    CHECK(fs->fs_lock(fs, f, &lock) == P9_LOCK_SUCCESS);
    lock.type = P9_LOCK_TYPE_UNLCK;
    CHECK(fs->fs_lock(fs, f, &lock) == P9_LOCK_SUCCESS);
    CHECK(fs->fs_mkdir(fs, &qid, root, "new", 0700, 0) == 0);
    CHECK(fs->fs_create(fs, &qid, root, "fifo", P9_O_WRONLY, 0600, 0) == -P9_EPERM);
    other = walk(fs, root, "fifo", 1);
    CHECK(fs->fs_setattr(fs, other, P9_SETATTR_SIZE, 0, 0, 0, 0, 0, 0, 0, 0) == -P9_EPERM);
    fs->fs_delete(fs, other);
    other = walk(fs, root, "new", 1);
    CHECK(fs->fs_create(fs, &qid, other, "child", P9_O_RDWR, 0600, 0) == 0);
    CHECK(fs->fs_write(fs, other, 0, (uint8_t *)"child", 5) == 5);
    fs->fs_delete(fs, other);
    other = walk(fs, root, "new", 1);
    CHECK(fs->fs_renameat(fs, other, "child", root, "child") == 0);
    CHECK(fs->fs_unlinkat(fs, root, "child") == 0);
    CHECK(fs->fs_unlinkat(fs, root, "new") == 0);
    fs->fs_delete(fs, other);
    fs->fs_delete(fs, f);
    f = walk(fs, root, "escape", 1);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_WRONLY | P9_O_TRUNC, NULL, NULL) < 0);
    CHECK(fs->fs_setattr(fs, f, P9_SETATTR_SIZE, 0, 0, 0, 0, 0, 0, 0, 0) < 0);
    fs->fs_delete(fs, f);
    /* An existing fid must not follow a replacement symlink or truncate its target. */
    f = walk(fs, root, "data", 1);
    CHECK(unlink(file) == 0);
    CHECK(symlink(outside, file) == 0);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_WRONLY | P9_O_TRUNC, NULL, NULL) < 0);
    int check_fd = open(outside, O_RDONLY);
    CHECK(check_fd >= 0 && read(check_fd, buf, sizeof(buf)) == 6);
    CHECK(memcmp(buf, "secret", 6) == 0);
    close(check_fd);
    CHECK(unlink(file) == 0);
    put_file(file, "edited");
    fs->fs_delete(fs, f);
    /* Intermediate links must not allow walking into a directory outside root. */
    CHECK(unlink(linkpath) == 0);
    CHECK(symlink(dir, linkpath) == 0);
    f = walk(fs, root, "escape", 1);
    other = walk(fs, f, "secret", 0);
    fs->fs_delete(fs, other);
    fs->fs_delete(fs, f);
    fs->fs_delete(fs, root);
    fs_end(fs);

    /* A single-file export has a synthetic root and must not expose siblings. */
    fs = fs_confined_init(file, FALSE);
    CHECK(fs);
    CHECK(fs->fs_attach(fs, &root, &qid, 0, "", "") == 0);
    CHECK(qid.type == P9_QTDIR);
    f = walk(fs, root, "secret", 0);
    fs->fs_delete(fs, f);
    f = walk(fs, root, "data", 1);
    CHECK(fs->fs_open(fs, &qid, f, P9_O_RDWR, NULL, NULL) == 0);
    CHECK(fs->fs_read(fs, f, 0, buf, sizeof(buf)) == 6);
    CHECK(memcmp(buf, "edited", 6) == 0);
    CHECK(fs->fs_open(fs, &qid, root, P9_O_RDONLY | P9_O_DIRECTORY, NULL, NULL) == 0);
    int n = fs->fs_readdir(fs, root, 0, buf, sizeof(buf));
    CHECK(n == 28 && memcmp(buf + 24, "data", 4) == 0);
    CHECK(fs->fs_readdir(fs, root, 1, buf, sizeof(buf)) == 0);
    CHECK(fs->fs_create(fs, &qid, root, "sibling", P9_O_RDWR, 0600, 0) < 0);
    CHECK(fs->fs_setattr(fs, root, P9_SETATTR_MODE, 0777, 0, 0, 0, 0, 0, 0, 0) < 0);
    CHECK(fs->fs_unlinkat(fs, root, "data") < 0);
    fs->fs_delete(fs, f);
    fs->fs_delete(fs, root);
    fs_end(fs);
    fs = fs_confined_init(outside, TRUE);
    CHECK(fs);
    fs_end(fs);
    unlink(fifo);
    unlink(linkpath);
    unlink(file);
    unlink(outside);
    rmdir(root_path);
    rmdir(dir);
    puts("Confined mappings enforce read-only and reject host path escape");
    return 0;
}
