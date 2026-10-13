/* POSIX launch bundle host operations. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "workspace_host.h"
#include "host_io.h"

static void remove_contents(int fd)
{
    DIR *dir = fdopendir(dup(fd));
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        int child = openat(fd, de->d_name, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (child >= 0) {
            remove_contents(child);
            close(child);
            unlinkat(fd, de->d_name, AT_REMOVEDIR);
        } else {
            unlinkat(fd, de->d_name, 0);
        }
    }
    closedir(dir);
}

void workspace_host_cleanup(const char *bundle)
{
    int fd = open(bundle, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd >= 0) {
        remove_contents(fd); close(fd); rmdir(bundle);
    }
}

static BOOL contains_path(const char *root, const char *path)
{
    size_t n = strlen(root);
    return !strcmp(root, "/") ||
        (!strncmp(root, path, n) && (path[n] == '/' || path[n] == 0));
}

/* A read-only export must not be reachable through any writable export. */
char *workspace_host_create_bundle(VirtMachineParams *p, const char *cwd)
{
    const char *parents[] = {"/tmp", "/var/tmp", getenv("XDG_RUNTIME_DIR"), getenv("HOME")};
    for (size_t i = 0; i < sizeof(parents)/sizeof(parents[0]); i++) {
        if (!parents[i]) continue;
        char *parent = realpath(parents[i], NULL);
        if (!parent) continue;
        BOOL overlap = cwd && contains_path(cwd, parent);
        for (int j = 0; j < p->fs_count && !overlap; j++) {
            if (!p->tab_fs[j].read_only) {
                char *root = realpath(p->tab_fs[j].filename, NULL);
                if (!root) overlap = TRUE;
                else { overlap = contains_path(root, parent); free(root); }
            }
        }
        char *temp = NULL;
        if (!overlap && asprintf(&temp, "%s/k-vm-launch-XXXXXX", parent) < 0) abort();
        free(parent);
        if (temp && mkdtemp(temp)) return temp;
        free(temp);
    }
    fprintf(stderr, "k-vm sandbox: no private launch directory outside writable mappings; narrow the mapped directory\n");
    return NULL;
}

int workspace_host_copy_config(WorkspaceDir source_dir, WorkspaceDir dest_dir, const char *name)
{
    /* Reject symlinks and special files; the directory alone is never exported. */
    int source=host_open_file(source_dir,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW);
    if(source<0) return errno==ENOENT ? 0 : -1;
    struct stat st;
    if(fstat(source,&st) || !S_ISREG(st.st_mode) || st.st_size>4*1024*1024) {
        close(source); errno=EINVAL; return -1;
    }
    int dest=openat(dest_dir,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(dest<0) {close(source);return -1;}
    char buf[8192]; ssize_t n; int result=0; size_t total=0;
    while((n=read(source,buf,sizeof(buf)))!=0) {
        if(n<0) {if(errno==EINTR)continue;result=-1;break;}
        total+=(size_t)n;
        if(total>4*1024*1024){errno=EFBIG;result=-1;break;}
        ssize_t done=0;
        while(done<n) {ssize_t w=write(dest,buf+done,n-done); if(w<0&&errno==EINTR)continue; if(w<=0){result=-1;break;}done+=w;}
        if(result)break;
    }
    close(source); close(dest); return result;
}

BOOL workspace_host_update_succeeded(const char *bundle)
{
    int dir = open(bundle, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir < 0) return FALSE;
    int result_dir = openat(dir, "result", O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    close(dir);
    if (result_dir < 0) return FALSE;
    int fd = openat(result_dir, "status", O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    close(result_dir);
    if (fd < 0) return FALSE;
    char status[16];
    ssize_t n = read(fd, status, sizeof(status));
    close(fd);
    return n == 2 && status[0] == '0' && status[1] == '\n';
}

WorkspaceDir workspace_host_open_dir(const char *path) { return open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); }
void workspace_host_close_dir(WorkspaceDir dir) { close(dir); }
int workspace_host_mkdir(const char *path) { return mkdir(path,0700); }
char *workspace_host_cwd(void) { return getcwd(NULL,0); }
unsigned long workspace_host_pid(void) { return getpid(); }
char *workspace_host_home(void) { const char *home=getenv("HOME");return home?strdup(home):NULL; }
FILE *workspace_host_script(const char *path) { return fopen(path,"wb"); }
