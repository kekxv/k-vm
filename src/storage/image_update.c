/* Atomic local raw image updates. SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#if defined(__linux__) && !defined(K_VM_PORTABLE_CLONE)
#include <linux/fs.h>
#endif
#include <signal.h>
#include "image_update.h"

static volatile sig_atomic_t cancelled;
void image_update_cancel(void) { cancelled = 1; }

struct ImageUpdate { char *original, *stage; int source; struct stat identity; };

static int copy_extent(int source, int target, off_t start, off_t end)
{
    char buf[65536];
    while (start < end) {
        if (cancelled) {errno = EINTR; return -1;}
        size_t size = end-start < (off_t)sizeof(buf) ? end-start : sizeof(buf);
        ssize_t n=pread(source,buf,size,start);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        ssize_t done=0;
        while(done<n){ssize_t w=pwrite(target,buf+done,n-done,start+done);
            if(w<0&&errno==EINTR)continue;
            if(w<=0)return -1;
            done+=w;}
        start+=n;
    }
    return 0;
}

static int clone_sparse(int source, int target, off_t size)
{
#if defined(__linux__) && !defined(K_VM_PORTABLE_CLONE)
    if (ioctl(target,FICLONE,source)==0) return 0;
#endif
    if (ftruncate(target,size)) return -1;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    off_t cursor=0;
    while(cursor<size){
        off_t data=lseek(source,cursor,SEEK_DATA);
        if(data<0){
            if(errno==ENXIO)return 0;
            if(errno==EINVAL)return copy_extent(source,target,0,size);
            return -1;
        }
        off_t hole=lseek(source,data,SEEK_HOLE);
        if(hole<0)return -1;
        if(hole>size)hole=size;
        if(copy_extent(source,target,data,hole))return -1;
        cursor=hole;
    }
    return 0;
#else
    return copy_extent(source,target,0,size);
#endif
}

ImageUpdate *image_update_begin(const char *path)
{
    cancelled = 0;
    ImageUpdate *u=calloc(1,sizeof(*u));
    if(!u)return NULL;
    u->source=-1;
    u->original=realpath(path,NULL);
    if(!u->original)goto fail;
    u->source=open(u->original,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(u->source<0||flock(u->source,LOCK_EX|LOCK_NB)||fstat(u->source,&u->identity))goto fail;
    struct stat current;
    if(!S_ISREG(u->identity.st_mode)||u->identity.st_size<=0||u->identity.st_size%512||
       stat(u->original,&current)||current.st_ino!=u->identity.st_ino||current.st_dev!=u->identity.st_dev){errno=EINVAL;goto fail;}
    if(asprintf(&u->stage,"%s.update-XXXXXX",u->original)<0)abort();
    int target=mkstemp(u->stage);
    if(target<0)goto fail;
    int error=fchmod(target,u->identity.st_mode&0777)||
        clone_sparse(u->source,target,u->identity.st_size)||fsync(target);
    close(target);
    if(error)goto fail;
    return u;
fail:
    {int saved=errno;image_update_finish(u,FALSE);errno=saved;return NULL;}
}

const char *image_update_path(ImageUpdate *u) { return u->stage; }

int image_update_finish(ImageUpdate *u, BOOL success)
{
    if(!u)return 0;
    int result=0;
    if(success){
        struct stat current;
        char *parent=strdup(u->original),*slash=strrchr(parent,'/');
        *slash=0;
        int dir=open(*parent?parent:"/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        free(parent);
        int fd=open(u->stage,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0||dir<0||fsync(fd)||fsync(dir)||stat(u->original,&current)||
           current.st_dev!=u->identity.st_dev||current.st_ino!=u->identity.st_ino){result=-1;}
        if(fd>=0)close(fd);
        /* Rename is the commit point. Defer cancellation signals while the
           final cancellation check and rename form one transaction. */
        sigset_t blocked, previous, pending;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGTERM);
        sigemptyset(&pending);
        int masked = sigprocmask(SIG_BLOCK, &blocked, &previous) == 0;
        if (!masked) result = -1;
        sigpending(&pending);
        if(cancelled || sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)) {
            errno=EINTR; result=-1;
        }
        if(!result && rename(u->stage,u->original))result=-1;
        if (masked) sigprocmask(SIG_SETMASK, &previous, NULL);
        /* Once rename succeeds, the update is committed. A durability warning
           must not pretend the original is still present. */
        if(!result && fsync(dir))
            fprintf(stderr,"k-vm update: image committed, but directory durability sync failed: %s\n",strerror(errno));
        if(dir>=0)close(dir);
    }
    if(u->stage)unlink(u->stage);
    if(u->source>=0)close(u->source);
    free(u->original);free(u->stage);free(u);
    return result;
}
