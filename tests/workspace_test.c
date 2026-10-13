/* Launch bundle exposes only named agent config files, never sessions or HOME. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "workspace.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void put(const char *path, const char *text) {
    FILE *f = fopen(path,"w"); CHECK(f); CHECK(fputs(text,f)>=0); fclose(f);
}
int main(void) {
    char base[]="/tmp/k-vm-workspace-XXXXXX", path[1024];
    CHECK(mkdtemp(base));
    snprintf(path,sizeof(path),"%s/config",base); CHECK(mkdir(path,0700)==0);
    char *config=strdup(path);
    snprintf(path,sizeof(path),"%s/config/config.toml",base); put(path,"model='test'\n");
    snprintf(path,sizeof(path),"%s/config/auth.json",base); put(path,"{\"token\":\"test\"}\n");
    snprintf(path,sizeof(path),"%s/config/history.jsonl",base); put(path,"private\n");
    snprintf(path,sizeof(path),"%s/config/sessions",base); CHECK(mkdir(path,0700)==0);
    VirtMachineParams params={0};
    const char *args[]={"--model","quote' argument"};
    CHECK(workspace_prepare(&params,"pi",config,2,args)==0);
    CHECK(params.fs_count==2);
    CHECK(!strcmp(params.tab_fs[0].tag,"work"));
    CHECK(!params.tab_fs[0].read_only && params.tab_fs[0].confined);
    CHECK(!strcmp(params.tab_fs[1].tag,"kv_launch") && params.tab_fs[1].read_only);
    char *bundle=strdup(params.tab_fs[1].filename);
    snprintf(path,sizeof(path),"%s/config/config.toml",bundle); CHECK(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/config/auth.json",bundle); CHECK(access(path,R_OK)==0);
    snprintf(path,sizeof(path),"%s/config/history.jsonl",bundle); CHECK(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/config/sessions",bundle); CHECK(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/entry.sh",bundle);
    FILE *f=fopen(path,"r"); CHECK(f); char script[16384]; size_t n=fread(script,1,sizeof(script)-1,f); script[n]=0; fclose(f);
    CHECK(strstr(script,"cd /work")); CHECK(!strstr(script,"CODEX_HOME")); CHECK(strstr(script,"PI_CODING_AGENT_DIR"));
    CHECK(strstr(script,"'quote'\\'' argument'"));
    workspace_cleanup(); CHECK(access(bundle,F_OK)!=0); free(bundle);
    for(int i=0;i<params.fs_count;i++){free(params.tab_fs[i].tag);free(params.tab_fs[i].filename);}
    memset(&params,0,sizeof(params));
    CHECK(workspace_prepare(&params,"shell",config,0,NULL)==0);
    bundle=strdup(params.tab_fs[1].filename);
    snprintf(path,sizeof(path),"%s/config/auth.json",bundle); CHECK(access(path,F_OK)!=0);
    workspace_cleanup(); free(bundle);
    for(int i=0;i<params.fs_count;i++){free(params.tab_fs[i].tag);free(params.tab_fs[i].filename);}
    memset(&params,0,sizeof(params));
    snprintf(path,sizeof(path),"%s/settings.json",config); put(path,"{}\n");
    snprintf(path,sizeof(path),"%s/models.json",config); put(path,"{}\n");
    CHECK(workspace_prepare(&params,"pi",config,0,NULL)==0);
    bundle=strdup(params.tab_fs[1].filename);
    snprintf(path,sizeof(path),"%s/config/settings.json",bundle); CHECK(access(path,R_OK)==0);
    snprintf(path,sizeof(path),"%s/config/models.json",bundle); CHECK(access(path,R_OK)==0);
    snprintf(path,sizeof(path),"%s/config/config.toml",bundle); CHECK(access(path,F_OK)!=0);
    workspace_cleanup(); free(bundle);
    for(int i=0;i<params.fs_count;i++){free(params.tab_fs[i].tag);free(params.tab_fs[i].filename);}
    memset(&params,0,sizeof(params));
    snprintf(path,sizeof(path),"%s/auth.json",config); CHECK(unlink(path)==0);
    CHECK(symlink("config.toml",path)==0);
    CHECK(workspace_prepare(&params,"pi",config,0,NULL)<0);
    CHECK(workspace_prepare(&params,"codex",config,0,NULL)<0);
    CHECK(workspace_prepare(&params,"invalid",config,0,NULL)<0);
    CHECK(chdir("/tmp")==0);
    /* Bazel may allow writes only under /tmp: safe refusal is valid too. */
    if (workspace_prepare(&params,"shell",config,0,NULL)==0)
        CHECK(strncmp(params.tab_fs[1].filename,"/tmp/",5)!=0);
    workspace_cleanup();
    for(int i=0;i<params.fs_count;i++){free(params.tab_fs[i].tag);free(params.tab_fs[i].filename);}
    memset(&params,0,sizeof(params));
    CHECK(chdir("/")==0);
    CHECK(workspace_prepare(&params,"shell",config,0,NULL)<0);
    CHECK(workspace_prepare_update(&params,"1.2.3;bad")<0);
    CHECK(workspace_prepare_update(&params,"1.2.3")==0);
    CHECK(params.fs_count==2);
    CHECK(!strcmp(params.tab_fs[0].tag,"kv_result")&&!params.tab_fs[0].read_only);
    CHECK(!strcmp(params.tab_fs[1].tag,"kv_launch")&&params.tab_fs[1].read_only);
    snprintf(path,sizeof(path),"%s/entry.sh",params.tab_fs[1].filename);
    f=fopen(path,"r");CHECK(f);n=fread(script,1,sizeof(script)-1,f);script[n]=0;fclose(f);
    CHECK(strstr(script,"@earendil-works/pi-coding-agent@1.2.3"));
    CHECK(!strstr(script," work /work"));CHECK(!strstr(script,"PI_CODING_AGENT_DIR"));
    CHECK(!workspace_update_succeeded());
    snprintf(path,sizeof(path),"%s/status",params.tab_fs[0].filename);
    put(path,"0\n");CHECK(workspace_update_succeeded());
    put(path,"1\n");CHECK(!workspace_update_succeeded());
    workspace_cleanup();
    for(int i=0;i<params.fs_count;i++){free(params.tab_fs[i].tag);free(params.tab_fs[i].filename);}
    free(config);
    puts("Agent launch bundle contains only allowlisted configuration");
    return 0;
}
