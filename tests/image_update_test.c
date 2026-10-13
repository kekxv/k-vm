#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <string.h>
#include <signal.h>
#include "image_update.h"
#define CHECK(c) do {if(!(c)){perror(#c);exit(1);}}while(0)
int main(void) {
    char path[]="/tmp/k-vm-update-test-XXXXXX", data[4];
    int fd=mkstemp(path); CHECK(fd>=0); CHECK(write(fd,"old\n",4)==4);
    CHECK(ftruncate(fd,(off_t)1<<30)==0);
    CHECK(flock(fd,LOCK_SH|LOCK_NB)==0); CHECK(!image_update_begin(path));
    CHECK(flock(fd,LOCK_UN)==0);
    ImageUpdate *tx=image_update_begin(path);CHECK(tx);
    char *stage=strdup(image_update_path(tx));
    int out=open(stage,O_WRONLY);CHECK(out>=0);CHECK(write(out,"new\n",4)==4);close(out);
    CHECK(image_update_finish(tx,FALSE)==0);CHECK(access(stage,F_OK)<0);free(stage);
    CHECK(pread(fd,data,4,0)==4&&!memcmp(data,"old\n",4));
    /* A cancellation already queued at commit must preserve the base disk. */
    sigset_t signals, previous;
    sigemptyset(&signals); sigaddset(&signals,SIGINT);
    CHECK(sigprocmask(SIG_BLOCK,&signals,&previous)==0);
    tx=image_update_begin(path);CHECK(tx);
    CHECK(raise(SIGINT)==0);
    CHECK(image_update_finish(tx,TRUE)<0);
    CHECK(pread(fd,data,4,0)==4&&!memcmp(data,"old\n",4));
    int pending_signal;
    CHECK(sigwait(&signals,&pending_signal)==0 && pending_signal==SIGINT);
    CHECK(sigprocmask(SIG_SETMASK,&previous,NULL)==0);
    tx=image_update_begin(path);CHECK(tx);
    out=open(image_update_path(tx),O_WRONLY);CHECK(out>=0);CHECK(write(out,"new\n",4)==4);close(out);
    CHECK(image_update_finish(tx,TRUE)==0);
    out=open(path,O_RDONLY);CHECK(out>=0);CHECK(read(out,data,4)==4&&!memcmp(data,"new\n",4));close(out);
    CHECK(pread(fd,data,4,0)==4&&!memcmp(data,"old\n",4));close(fd);unlink(path);
    puts("Image updates lock, preserve original on failure, and commit atomically");
}
