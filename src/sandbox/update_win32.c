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
static ImageUpdate *transaction;
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
int sandbox_update_run(VirtMachineParams *p)
{
    if (transaction) {
        errno = EBUSY;
        return 1;
    }
    InterlockedExchange(&interrupted, 0);
    if (!SetConsoleCtrlHandler(interrupt_update, TRUE)) {
        fprintf(stderr, "k-vm update: cannot install cancellation handler\n");
        return 1;
    }
    transaction = image_update_begin(p->tab_drive[0].filename);
    if (!transaction || InterlockedCompareExchange(&interrupted, 0, 0)) {
        if (transaction) {
            image_update_finish(transaction, FALSE);
            transaction = NULL;
        } else
            perror("k-vm update: cannot stage disk (image may be in use)");
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
    SetConsoleCtrlHandler(interrupt_update, FALSE);
    if (result) {
        perror("k-vm update: commit failed; original image preserved");
        return 1;
    }
    if (!success) {
        fprintf(stderr,
                "k-vm update: guest update failed or interrupted; original image preserved\n");
        return 1;
    }
    puts("k-vm update: Pi updated in the local image");
    return 0;
}
