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
#include "initialize.h"

static volatile sig_atomic_t update_child;
static volatile sig_atomic_t interrupted;
static void interrupt_update(int signal_number)
{
    interrupted=1;
    image_update_cancel();
    if(update_child>0)kill(update_child,signal_number);
}

int sandbox_update_run(VirtMachineParams *p, const char *operation)
{
    struct sigaction action={0},old_int,old_term;
    action.sa_handler=interrupt_update;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,&old_int);sigaction(SIGTERM,&action,&old_term);
    ImageUpdate *transaction=image_update_begin(p->tab_drive[0].filename);
    if(!transaction || interrupted){
        if(transaction) image_update_finish(transaction,FALSE);
        else fprintf(stderr, "k-vm %s: cannot stage disk (image may be in use): %s\n", operation, strerror(errno));
        sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
        return 1;
    }
    char *original_disk = strdup(p->tab_drive[0].filename);
    if (!original_disk) {
        image_update_finish(transaction, FALSE);
        sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
        return 1;
    }
    free(p->tab_drive[0].filename);
    p->tab_drive[0].filename=strdup(image_update_path(transaction));
    pid_t child=fork();
    if(child<0){perror("fork");image_update_finish(transaction,FALSE);
        sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);free(original_disk);return 1;}
    if(child==0){
        free(original_disk);
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
    if (!committed && success && sandbox_pi_record_initialized(original_disk))
        fprintf(stderr, "k-vm: install succeeded, but readiness hint could not be saved; next Pi launch will verify installation again\n");
    free(original_disk);
    sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
    if(committed){fprintf(stderr,"k-vm %s: commit failed; original image preserved: %s\n",operation,strerror(errno));return 1;}
    if(!success){fprintf(stderr,"k-vm %s: guest installation failed or interrupted; original image preserved\n",operation);return 1;}
    printf("k-vm %s: Pi %s in the local image\n", operation, !strcmp(operation,"init") ? "initialized" : "updated");
    return 0;
}
