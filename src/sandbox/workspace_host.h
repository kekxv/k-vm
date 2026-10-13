/* Platform operations for private launch bundles. SPDX-License-Identifier: MIT */
#ifndef K_VM_WORKSPACE_HOST_H
#define K_VM_WORKSPACE_HOST_H
#include <stdint.h>
#include <stdio.h>
#include "workspace.h"
typedef intptr_t WorkspaceDir;
char *workspace_host_create_bundle(VirtMachineParams *p,const char *cwd);
void workspace_host_cleanup(const char *bundle);
WorkspaceDir workspace_host_open_dir(const char *path);
void workspace_host_close_dir(WorkspaceDir dir);
int workspace_host_copy_config(WorkspaceDir source,WorkspaceDir dest,const char *name);
BOOL workspace_host_update_succeeded(const char *bundle);
int workspace_host_mkdir(const char *path);
char *workspace_host_cwd(void);
unsigned long workspace_host_pid(void);
char *workspace_host_home(void);
FILE *workspace_host_script(const char *path);
#endif
