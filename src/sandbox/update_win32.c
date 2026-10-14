/* Run the staged guest in this process; commit after VM disks close. SPDX-License-Identifier: MIT
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "update.h"
#include "workspace.h"
#include "image_update.h"
#include "initialize.h"
static ImageUpdate *transaction;
static char *original_disk;
static const char *active_operation;
static volatile LONG interrupted;
static BOOL cleanup_registered;
static void rollback_at_exit(void)
{
    if (transaction) {
        image_update_finish(transaction, FALSE);
        transaction = NULL;
    }
}
static BOOL WINAPI interrupt_update(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT &&
        event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT)
        return FALSE;
    InterlockedExchange(&interrupted, 1);
    image_update_cancel();
    /* During staging, consume the event so cancellation can remove the stage.
       The VM installs its own handler later and requests guest shutdown there. */
    return TRUE;
}
int sandbox_update_run(VirtMachineParams *p, const char *operation)
{
    active_operation = operation;
    if (transaction) {
        errno = EBUSY;
        return 1;
    }
    InterlockedExchange(&interrupted, 0);
    if (!SetConsoleCtrlHandler(interrupt_update, TRUE)) {
        fprintf(stderr, "k-vm %s: cannot install cancellation handler\n", operation);
        return 1;
    }
    transaction = image_update_begin(p->tab_drive[0].filename);
    if (!transaction || InterlockedCompareExchange(&interrupted, 0, 0)) {
        if (transaction) {
            image_update_finish(transaction, FALSE);
            transaction = NULL;
        } else
            fprintf(stderr, "k-vm %s: cannot stage disk (image may be in use): %s\n", operation, strerror(errno));
        SetConsoleCtrlHandler(interrupt_update, FALSE);
        return 1;
    }
    if (!cleanup_registered) {
        if (atexit(rollback_at_exit)) {
            image_update_finish(transaction, FALSE);
            transaction = NULL;
            SetConsoleCtrlHandler(interrupt_update, FALSE);
            return 1;
        }
        cleanup_registered = TRUE;
    }
    char *stage = strdup(image_update_path(transaction));
    if (!stage) {
        image_update_finish(transaction, FALSE);
        transaction = NULL;
        SetConsoleCtrlHandler(interrupt_update, FALSE);
        return 1;
    }
    original_disk = strdup(p->tab_drive[0].filename);
    if (!original_disk) {
        free(stage);
        image_update_finish(transaction, FALSE);
        transaction = NULL;
        SetConsoleCtrlHandler(interrupt_update, FALSE);
        return 1;
    }
    free(p->tab_drive[0].filename);
    p->tab_drive[0].filename = stage;
    return -1;
}
int sandbox_update_complete(int vm_status)
{
    if (!transaction)
        return vm_status;
    BOOL success = vm_status == 0 && !InterlockedCompareExchange(&interrupted, 0, 0) &&
                   workspace_update_succeeded();
    ImageUpdate *u = transaction;
    transaction = NULL;
    int result = image_update_finish(u, success);
    if (!result && success && sandbox_pi_record_initialized(original_disk))
        fprintf(stderr, "k-vm: install succeeded, but readiness hint could not be saved; next Pi launch will verify installation again\n");
    free(original_disk); original_disk = NULL;
    SetConsoleCtrlHandler(interrupt_update, FALSE);
    if (result) {
        fprintf(stderr, "k-vm %s: commit failed; original image preserved: %s\n", active_operation, strerror(errno));
        return 1;
    }
    if (!success) {
        fprintf(stderr,
                "k-vm %s: guest installation failed or interrupted; original image preserved\n", active_operation);
        return 1;
    }
    printf("k-vm %s: Pi %s in the local image\n", active_operation,
           !strcmp(active_operation,"init") ? "initialized" : "updated");
    return 0;
}
