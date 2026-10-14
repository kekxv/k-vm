/* Minimal agent configuration and current-directory launch policy. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include "workspace.h"
#include "workspace_host.h"
#include "initialize.h"
#ifdef _WIN32
#include "win32_files.h"
#endif

static char *active_bundle;
static unsigned long bundle_owner;
static const char *pi_files[] = {"auth.json", "settings.json", "models.json", NULL};

void workspace_cleanup(void)
{
    if (!active_bundle || bundle_owner != workspace_host_pid()) return;
    workspace_host_cleanup(active_bundle);
    free(active_bundle); active_bundle = NULL;
}

static void quote(FILE *f, const char *s)
{
    fputc('\'',f);
    for(;*s;s++) {if(*s=='\'')fputs("'\\''",f);else fputc(*s,f);}
    fputc('\'',f);
}

static void export_path(VirtMachineParams *p, const char *tag, const char *path, BOOL ro)
{
    p->tab_fs[p->fs_count++]=(VMFSEntry){.tag=strdup(tag),.filename=strdup(path),.confined=TRUE,.read_only=ro};
}

int workspace_prepare(VirtMachineParams *p, const char *agent, const char *config_dir,
                      int argc, const char *const *argv)
{
    if(strcmp(agent,"shell")&&strcmp(agent,"pi")) {
        fprintf(stderr,"k-vm sandbox: agent must be shell or pi\n");return -1;
    }
    if(p->fs_count+2>MAX_FS_DEVICE) {fprintf(stderr,"k-vm sandbox: workspace requires two free 9P slots\n");return -1;}
    for(int i=0;i<p->fs_count;i++)if(!strcmp(p->tab_fs[i].tag,"work")||!strcmp(p->tab_fs[i].tag,"kv_launch")) {
        fprintf(stderr,"k-vm sandbox: reserved mapping tag: %s\n",p->tab_fs[i].tag);return -1;
    }
    char *cwd=workspace_host_cwd(); if(!cwd)return -1;
    if(active_bundle) {free(cwd);return -1;}
    active_bundle=workspace_host_create_bundle(p,cwd);
    if(!active_bundle) {free(cwd);return -1;}
    bundle_owner=workspace_host_pid();
    const char *temp=active_bundle;
    atexit(workspace_cleanup);
    char *path;
    if(asprintf(&path,"%s/config",temp)<0)abort();
    if(workspace_host_mkdir(path)) {free(path);free(cwd);workspace_cleanup();return -1;}
    WorkspaceDir dest=workspace_host_open_dir(path); free(path);
    if(dest<0) {free(cwd);workspace_cleanup();return -1;}
    const char **files=pi_files;
    if(strcmp(agent,"shell")) {
        char *default_dir=NULL;
#ifdef _WIN32
        char *environment_dir=NULL;
#endif
        if(!config_dir) {
            char *home=workspace_host_home();
#ifdef _WIN32
            config_dir=environment_dir=kv_win32_getenv_utf8("PI_CODING_AGENT_DIR");
#else
            config_dir=getenv("PI_CODING_AGENT_DIR");
#endif
            if(!config_dir&&home) {
                if(asprintf(&default_dir,"%s/%s",home,".pi/agent")<0)abort();
                config_dir=default_dir;
            }
            free(home);
        }
        WorkspaceDir source=config_dir?workspace_host_open_dir(config_dir):-1;
        if(source<0 && (config_dir && (errno!=ENOENT || default_dir==NULL))) {
            fprintf(stderr,"k-vm sandbox: cannot read agent config directory\n");free(default_dir);workspace_host_close_dir(dest);free(cwd);workspace_cleanup();return -1;
        }
        if(source>=0) {
            for(int i=0;files[i];i++) if(workspace_host_copy_config(source,dest,files[i])) {
                fprintf(stderr,"k-vm sandbox: unsafe or unreadable config file: %s\n",files[i]);
                workspace_host_close_dir(source);workspace_host_close_dir(dest);free(default_dir);free(cwd);workspace_cleanup();return -1;
            }
            workspace_host_close_dir(source);
        }
        free(default_dir);
#ifdef _WIN32
        free(environment_dir);
#endif
    }
    workspace_host_close_dir(dest);
    if(asprintf(&path,"%s/entry.sh",temp)<0)abort();
    FILE *entry=workspace_host_script(path);free(path);
    if(!entry){free(cwd);workspace_cleanup();return -1;}
    fputs("#!/bin/sh\nset -eu\nexport HOME=/root TERM=xterm-256color\nmkdir -p /work\n"
          "mount -t 9p -o trans=virtio,version=9p2000.L,msize=8192 work /work\ncd /work\nexport PS1='$PWD # '\n",entry);
    fprintf(entry,"date -s @%lld >/dev/null\n",(long long)time(NULL));
    if (p->eth_count && !strcmp(p->tab_eth[0].driver,"user"))
        fputs("ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up\nroute add default gw 10.0.2.2\nprintf 'nameserver 10.0.2.3\\n' > /etc/resolv.conf\n",entry);
    if(strcmp(agent,"shell")) {
        fputs("export PI_CODING_AGENT_DIR=/root/.pi/agent\nmkdir -p \"$PI_CODING_AGENT_DIR\"\n",entry);
        for(int i=0;files[i];i++) {
            fprintf(entry,"if [ -f /run/k-vm/config/%s ]; then cp /run/k-vm/config/%s \"$%s/%s\"; fi\n",
                    files[i],files[i],"PI_CODING_AGENT_DIR",files[i]);
        }
        fputs("chmod -R go-rwx /root/.pi 2>/dev/null || true\n",entry);
        fputs("if ! command -v ",entry);quote(entry,agent);
        fputs(" >/dev/null 2>&1; then echo 'Selected agent is not installed in this guest image.' >&2; exit 127; fi\n",entry);
    }
    fputs("set +e\n",entry);
    if(!strcmp(agent,"shell"))fputs("/bin/sh -i",entry);else quote(entry,agent);
    for(int i=0;i<argc;i++){fputc(' ',entry);quote(entry,argv[i]);}
    fputs("\nstatus=$?\n",entry);
    if(strcmp(agent,"shell"))
        fputs("echo \"k-vm: agent ended ($status); entering shell\"\n/bin/sh -i\n",entry);
    fputs("sync\n/bin/busybox poweroff -f\n",entry);
    int failed=ferror(entry);if(fclose(entry))failed=1;
    if(failed){free(cwd);workspace_cleanup();return -1;}
    export_path(p,"work",cwd,FALSE);
    export_path(p,"kv_launch",temp,TRUE);
    free(cwd);return 0;
}


static int prepare_maintenance(VirtMachineParams *p, const char *version,
                               BOOL initialize, const char *packages, const char *node_version)
{
    if (active_bundle || p->fs_count || !version || !*version) return -1;
    for (const char *v = version; *v; v++)
        if (!((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'z') ||
              (*v >= 'A' && *v <= 'Z') || *v == '.' || *v == '-')) return -1;
    active_bundle = workspace_host_create_bundle(p, NULL);
    if (!active_bundle) return -1;
    bundle_owner = workspace_host_pid();
    atexit(workspace_cleanup);
    char *path;
    char *downloaded = NULL;
    if (initialize && node_version && !packages && p->eth_count &&
        p->drive_count == 1 && !sandbox_pi_initialized(p->tab_drive[0].filename)) {
        if (asprintf(&downloaded, "%s/node-packages", active_bundle) < 0) abort();
        if (workspace_host_mkdir(downloaded)) { free(downloaded); workspace_cleanup(); return -1; }
        fprintf(stderr, "k-vm init: fetching public Node.js archive on the host...\n");
        int fetched = sandbox_prefetch_node(node_version, downloaded);
        if (!fetched) packages = downloaded;
        else if (fetched == 127) {
            fputs("k-vm init: host curl unavailable; using guest HTTPS download\n", stderr);
        } else {
            fputs("k-vm init: host Node.js download failed; original image preserved\n", stderr);
            free(downloaded); workspace_cleanup(); return -1;
        }
    }
    if (asprintf(&path, "%s/result", active_bundle) < 0) abort();
    if (workspace_host_mkdir(path)) { free(path); workspace_cleanup(); return -1; }
    export_path(p, "kv_result", path, FALSE);
    free(path);
    if (packages) export_path(p, "kv_packages", packages, TRUE);
    BOOL have_packages = packages != NULL;
    free(downloaded);
    if (asprintf(&path, "%s/entry.sh", active_bundle) < 0) abort();
    FILE *entry = workspace_host_script(path);
    free(path);
    if (!entry) {workspace_cleanup(); return -1;}
    fputs("#!/bin/sh\nset -eu\nexport HOME=/root\n"
          "mkdir -p /run/k-vm-result\n"
          "mount -t 9p -o trans=virtio,version=9p2000.L,msize=8192 kv_result /run/k-vm-result\n", entry);
    fprintf(entry,"date -s @%lld >/dev/null\n",(long long)time(NULL));
    if (p->eth_count && !strcmp(p->tab_eth[0].driver, "user"))
        fputs("ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up\n"
              "route add default gw 10.0.2.2\nprintf 'nameserver 10.0.2.3\\n' > /etc/resolv.conf\n", entry);
    if (have_packages)
        fputs("mkdir -p /run/k-vm-packages\nmount -t 9p -o trans=virtio,version=9p2000.L,msize=8192,ro kv_packages /run/k-vm-packages\n", entry);
    /* Download/cache writes must not inflate the sparse persistent root disk. */
    fputs("mount -t tmpfs -o mode=1777,size=50% tmpfs /tmp\n", entry);
    fputs("set +e\n", entry);
    if (initialize) {
        fputs("/usr/local/sbin/k-vm-install-pi --version ", entry);
        quote(entry, version);
        if (have_packages) fputs(" --packages /run/k-vm-packages", entry);
        if (!p->eth_count) fputs(" --offline", entry);
        fputc('\n', entry);
    } else {
        /* Keep updates compatible with full images built before the installer existed. */
        fputs("export npm_config_cache=/tmp/k-vm-npm-cache\nnpm install -g ", entry);
        char *package;
        if (asprintf(&package, "@earendil-works/pi-coding-agent@%s", version) < 0) abort();
        quote(entry, package);
        free(package);
        fputs(" --no-progress --no-audit --no-fund\n", entry);
    }
    fputs("status=$?\nrm -rf /tmp/k-vm-npm-cache /root/.npm\nif [ $status -eq 0 ]; then pi --version; status=$?; fi\n"
          "sync\nprintf '%s\\n' \"$status\" > /run/k-vm-result/status\nsync\n/bin/busybox poweroff -f\n", entry);
    int failed = ferror(entry);
    if (fclose(entry)) failed = 1;
    if (failed) {workspace_cleanup(); return -1;}
    export_path(p, "kv_launch", active_bundle, TRUE);
    return 0;
}

int workspace_prepare_update(VirtMachineParams *p, const char *version)
{
    return prepare_maintenance(p, version, FALSE, NULL, NULL);
}

int workspace_prepare_init(VirtMachineParams *p, const char *version, const char *packages,
                           const char *node_version)
{
    return prepare_maintenance(p, version, TRUE, packages, node_version);
}

BOOL workspace_update_succeeded(void)
{
    return active_bundle && workspace_host_update_succeeded(active_bundle);
}
