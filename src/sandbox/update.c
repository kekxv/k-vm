/* Guest Pi updates are staged and committed only after verified success. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "update.h"
#include "workspace.h"
#include "image_update.h"

static volatile sig_atomic_t update_child;
static volatile sig_atomic_t interrupted;
static void interrupt_update(int signal_number)
{
    interrupted=1;
    image_update_cancel();
    if(update_child>0)kill(update_child,signal_number);
}

int sandbox_update_run(VirtMachineParams *p)
{
    struct sigaction action={0},old_int,old_term;
    action.sa_handler=interrupt_update;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,&old_int);sigaction(SIGTERM,&action,&old_term);
    ImageUpdate *transaction=image_update_begin(p->tab_drive[0].filename);
    if(!transaction || interrupted){
        if(transaction) image_update_finish(transaction,FALSE);
        else perror("k-vm update: cannot stage disk (image may be in use)");
        sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
        return 1;
    }
    free(p->tab_drive[0].filename);
    p->tab_drive[0].filename=strdup(image_update_path(transaction));
    pid_t child=fork();
    if(child<0){perror("fork");image_update_finish(transaction,FALSE);
        sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);return 1;}
    if(child==0){
        sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
        return -1;
    }
    update_child=child;
    if(interrupted)kill(child,SIGTERM);
    int status=0;pid_t waited;
    do {waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    update_child=0;
    BOOL success=waited==child&&!interrupted&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&workspace_update_succeeded();
    int committed=image_update_finish(transaction,success);
    sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
    if(committed){perror("k-vm update: commit failed; original image preserved");return 1;}
    if(!success){fprintf(stderr,"k-vm update: guest update failed or interrupted; original image preserved\n");return 1;}
    puts("k-vm update: Pi updated in the local image");
    return 0;
}
