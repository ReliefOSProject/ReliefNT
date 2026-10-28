#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/module/internal.h"
#include "kmodules/host_memory.h"
static int initialized[4],exited[4],stopped,released;
static int stop_ready=1,nested;
static int block_init,entered_init,proceed_init;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static struct { void *bytes; size_t size; } images[16];
static int stop_resource(void *p) { assert(p==&released);++stopped;return stop_ready ? 0 : -16; }
static void free_resource(void *p) { assert(p==&released);++released; }
int test_control(struct module *owner,int stage);
int test_control(struct module *owner,int stage)
{
    if(stage>=10){++exited[stage-10];return 0;}
    ++initialized[stage];
    assert(!try_module_get(owner)); /* COMING does not admit external users. */
    assert(!module_execution_get(owner));
    if(stage==2 || stage==3)assert(!module_add_resource(owner,stop_resource,free_resource,&released));
    if(nested && !stage){
        assert(module_load_image(images[0].bytes,images[0].size,"",0)==-17);
        assert(!module_load_image(images[3].bytes,images[3].size,"",0));
    }
    pthread_mutex_lock(&gate);
    if(block_init && !stage){entered_init=1;pthread_cond_broadcast(&condition);while(!proceed_init)pthread_cond_wait(&condition,&gate);}
    pthread_mutex_unlock(&gate);
    return 0;
}
struct module_parameter;
extern void module_layout(struct module *,const struct module_parameter *,const struct module_export_symbol *);
/** @brief Return an independently expected value from each optional host export.
 * @return Literal 42, checked by the loaded ELF fixture's real entry point.
 */
static int weak_fixture_value(void) { return 42; }
static const struct module_export_symbol kernel_exports[] __attribute__((section("__ksymtab"),used)) = {
    {(uintptr_t)test_control,"test_control",""},
    {(uintptr_t)module_layout,"module_layout",""},
    {(uintptr_t)weak_fixture_value,"weak_value",""},
    {(uintptr_t)weak_fixture_value,"weak_namespaced","RELIEFNT_WEAK_TEST"},
};
static const uint32_t kernel_crcs[] __attribute__((section("__kcrctab"),used)) = {0x11223344u,0x55667788u,0x12345678u,0x12345678u};
static const struct module_export_symbol kernel_gpl_exports[] __attribute__((section("__ksymtab_gpl"),used)) = {
    {(uintptr_t)weak_fixture_value,"weak_gpl",""},
};
static const uint32_t kernel_gpl_crcs[] __attribute__((section("__kcrctab_gpl"),used)) = {0x12345678u};
static void read_image(unsigned i,const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    images[i].size=(size_t)n;images[i].bytes=malloc((size_t)n);assert(images[i].bytes);
    assert(fread(images[i].bytes,1,(size_t)n,f)==(size_t)n);fclose(f);
}
static int load(unsigned i) { return module_load_image(images[i].bytes,images[i].size,"",0); }
static void *concurrent_loader(void *unused){(void)unused;return (void *)(intptr_t)load(0);}
/** @brief Exercise actual ELF linking, import policy, retirement and rollback.
 * @param argc Program plus the sixteen independently compiled module paths.
 * @param argv Borrowed host fixture paths; no host modules are loaded.
 * @return Zero after all real loader/owner assertions, abort on a broken contract.
 */
int main(int argc,char **argv)
{
    assert(argc==17);for(unsigned i=0;i<16;++i)read_image(i,argv[i+1]);
    assert(!load(8));assert(!module_unload("weak_absent",0));
    assert(!load(9));assert(!module_unload("weak_valid",0));
    assert(load(10)==-ENOEXEC);
    assert(load(11)==-ENOEXEC);
    assert(load(12)==-EINVAL);
    assert(load(13)==-EPERM);
    assert(!load(14));assert(!module_unload("weak_ns_valid",0));
    assert(!load(15));assert(!module_unload("weak_gpl_valid",0));
    assert(!host_live_heap && !host_live_maps);
    assert(try_module_get(NULL));module_put(NULL);assert(module_execution_get(NULL));module_execution_put(NULL);
    assert(module_unload("absent",0)==-2);
    assert(!load(0));assert(initialized[0]==1);
    assert(load(0)==-17);assert(initialized[0]==1);
    assert(!load(1));
    struct module_status status;
    assert(!module_status_at(0,&status) && !strcmp(status.name,"consumer") && status.size>0);
    char listing[1024]={0}; uint32_t listed=0;
    assert(!module_proc_read(0,listing,sizeof(listing)-1,&listed));
    assert(strstr(listing,"1 consumer, Live 0x0000000000000000"));
    for(uint32_t i=0;i<listed;++i){char byte=0;uint32_t got=0;assert(!module_proc_read(i,&byte,1,&got));assert(got==1 && byte==listing[i]);}
    char holder[MODULE_NAME_LEN];assert(!module_holder_at("provider",0,holder));assert(!strcmp(holder,"consumer"));
    assert(module_unload("provider",0)==-11);
    struct module *owner=module_get_by_name("consumer");assert(owner);
    assert(module_unload("consumer",0)==-11);module_put(owner);
    assert(!module_unload("consumer",0));assert(!module_unload("provider",0));
    assert(exited[0]==1 && exited[1]==1 && !host_live_heap && !host_live_maps);
    assert(load(1)==-2); /* provider dependency absent */
    assert(load(4)==-8); /* exact build identity mismatch */
    assert(load(5)==-8); /* imported symbol CRC mismatch */
    assert(load(7)==-8); /* modpost ABI anchor is checked even without an ELF relocation */
    assert(!host_live_heap && !host_live_maps);
    assert(load(2)==-22);assert(stopped==1 && released==1 && !exited[2]);
    stop_ready=0;assert(load(2)==-22);assert(host_live_maps && released==1);
    stop_ready=1;assert(!module_unload("init_fail",0));assert(released==2 && !exited[2]);
    assert(!host_live_heap && !host_live_maps);
    assert(!load(3));owner=module_get_by_name("busy");assert(owner);
    assert(module_execution_get(owner));module_put(owner);
    stop_ready=0;assert(module_unload("busy",0)==-16);
    assert(!try_module_get(owner) && !module_execution_get(owner));
    assert(host_live_maps && released==2);
    stop_ready=1;assert(module_unload("busy",0)==-16); /* active callback still owns code */
    module_execution_put(owner);assert(!module_unload("busy",0));
    assert(released==3 && exited[3]==1 && !host_live_heap && !host_live_maps);
    nested=1;assert(!load(0));nested=0;assert(!module_unload("busy",0));assert(!module_unload("provider",0));
    block_init=1;pthread_t thread;assert(!pthread_create(&thread,NULL,concurrent_loader,NULL));
    pthread_mutex_lock(&gate);while(!entered_init)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
    assert(load(0)==-17);assert(module_unload("provider",0)==-16);
    pthread_mutex_lock(&gate);proceed_init=1;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
    void *ret;assert(!pthread_join(thread,&ret));assert(!ret);block_init=0;
    assert(!module_unload("provider",0));assert(!host_live_heap && !host_live_maps);
    for(long fail=0;;++fail){
        host_fail_after=fail;int rc=load(0);host_fail_after=-1;
        if(!rc){assert(!module_unload("provider",0));break;}
        assert(rc==-12);assert(!host_live_heap && !host_live_maps);assert(fail<200);
    }
    /* Linux 6.12 ignores delete_module flag bits with force-unload disabled.
     * BusyBox supplies O_EXCL by default; none may bypass real external users. */
    const unsigned unload_flags[]={0x800u|0x80u,0x200u,0x80000000u,~0u};
    for(unsigned flag=0;flag<sizeof(unload_flags)/sizeof(unload_flags[0]);++flag) {
        assert(!load(0));struct module *held=module_get_by_name("provider");assert(held);
        assert(module_unload("provider",unload_flags[flag])==-EWOULDBLOCK);
        module_put(held);assert(!module_unload("provider",unload_flags[flag]));
        assert(!host_live_heap && !host_live_maps);
    }
    assert(module_load_image(images[6].bytes,images[6].size,"enabled signed-value=-2147483648 unsigned_value=0xffffffff label=\"hello world\"",0)==0);
    struct module_parameter_info parameter_info;
    assert(!module_parameter_at("parameters",0,&parameter_info) && !strcmp(parameter_info.name,"enabled") && parameter_info.permissions==0644);
    assert(!module_parameter_at("parameters",3,&parameter_info) && !strcmp(parameter_info.name,"runtime_label"));
    assert(module_parameter_at("parameters",4,&parameter_info)==-ENOENT);
    char parameter_text[80]={0};uint32_t parameter_bytes=0;
    assert(!module_parameter_read("parameters","enabled",0,parameter_text,sizeof(parameter_text),&parameter_bytes) && parameter_bytes==2 && !memcmp(parameter_text,"Y\n",2));
    assert(module_parameter_read("parameters","label",0,parameter_text,sizeof(parameter_text),&parameter_bytes)==-ENOENT);
    assert(module_parameter_write("parameters","signed_value","1",1,&parameter_bytes)==-EIO && !parameter_bytes);
    assert(!module_parameter_write("parameters","enabled","off\n",4,&parameter_bytes) && parameter_bytes==4);
    assert(!module_parameter_read("parameters","enabled",0,parameter_text,sizeof(parameter_text),&parameter_bytes) && !memcmp(parameter_text,"N\n",2));
    assert(!module_parameter_write("parameters","runtime_label","modified",8,&parameter_bytes));
    host_fail_after=0;
    assert(module_parameter_write("parameters","runtime_label","lost",4,&parameter_bytes)==-ENOMEM && !parameter_bytes);
    host_fail_after=1;
    assert(module_parameter_write("parameters","runtime_label","lost",4,&parameter_bytes)==-ENOMEM && !parameter_bytes);
    host_fail_after=-1;
    assert(!module_parameter_read("parameters","runtime_label",0,parameter_text,sizeof(parameter_text),&parameter_bytes) && parameter_bytes==9 && !memcmp(parameter_text,"modified\n",9));
    assert(!module_unload("parameters",0));
    assert(module_load_image(images[6].bytes,images[6].size,"unknown=1",0)==-2);
    assert(module_load_image(images[6].bytes,images[6].size,"signed_value=2147483648",0)==-34);
    assert(module_load_image(images[6].bytes,images[6].size,"unsigned_value=-1",0)==-34);
    assert(!host_live_heap && !host_live_maps);
    int before_retry=initialized[2];
    assert(module_load_image(images[2].bytes,images[2].size,"failure=-11",0)==-EAGAIN);
    assert(initialized[2]==before_retry+1 && !host_live_heap && !host_live_maps);
    char data[1024];uint32_t bytes=99;
    assert(!module_proc_read(0,data,sizeof(data),&bytes));assert(!bytes);
    assert(!load(0));
    struct module_identity *retired=module_identity_get("provider");
    assert(retired && module_identity_alive(retired));
    assert(module_identity_get("missing")==NULL);
    assert(!module_unload("provider",0) && !module_identity_alive(retired));
    assert(!host_live_maps && host_live_heap==1); /* Identity never pins code. */
    assert(!load(0));
    struct module_identity *fresh=module_identity_get("provider");
    assert(fresh && fresh!=retired && module_identity_alive(fresh));
    struct module_identity *same=module_identity_get("provider");assert(same==fresh);
    module_identity_put(same);module_identity_put(fresh);
    assert(!module_unload("provider",0) && !module_identity_alive(retired));
    module_identity_put(retired);module_identity_put(NULL);
    assert(!host_live_heap && !host_live_maps);
    for(unsigned i=0;i<16;++i)free(images[i].bytes);
    puts("PASS modules: dependency pins, duplicate/concurrent load, exact ABI/CRC, failed init, nested init, external busy, callback drain, safe STOP retention, rollback");
}
