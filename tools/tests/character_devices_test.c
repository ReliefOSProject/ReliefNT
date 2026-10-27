/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/chrdev.h>
#include "../../kernel/module/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio_fixture.h"
static size_t allocated;
static long fail_after=-1;
static int opens,closes,fail_open;
void *kernel_malloc(size_t bytes)
{
    if(fail_after==0)return NULL;
    if(fail_after>0)--fail_after;
    void *memory=calloc(1,bytes);if(memory)++allocated;return memory;
}
void kernel_free(void *memory){if(memory){assert(allocated);--allocated;free(memory);}}
void module_memory_free(struct module_memory *memory){assert(!memory);}
static struct module owner;
static struct character_device_registration registration;
static int device_open(struct task *task,struct task_file *file)
{
    assert(!task && !file);++opens;
    /* Reentrant registry access proves no registration lock spans callbacks. */
    assert(character_device_register(&registration)==-17);
    return fail_open ? -5 : 0;
}
static void device_close(struct task_file *file){assert(!file);++closes;}
static int64_t device_ioctl(struct task *task,struct task_file *file,uint64_t request,uint64_t argument)
{assert(!task && !file);return (int64_t)(request+argument);}
static int64_t device_readv(struct task *task,struct task_file *file,const struct iovec *vectors,
                            uint64_t count,bool positional,uint64_t position,uint64_t flags)
{assert(!task && !file && !vectors && count==0 && positional && position==31 && flags==7);return 19;}
static const struct character_file_operations operations={.open=device_open,.close=device_close,.ioctl=device_ioctl,.readv=device_readv};
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(owner.name,"chr_provider");strcpy(runtime->name,owner.name);owner.runtime=runtime;runtime->owner=&owner;
    runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    registration=(struct character_device_registration){.major=116,.first_minor=0,.count=256,.owner=&owner,.operations=&operations};
    assert(!character_device_register(&registration));
    assert(character_device_register(&registration)==-17);
    struct character_file *handle=NULL;
    assert(character_file_open(116u<<8,NULL,NULL,&handle)==-19 && !handle && !runtime->users);
    runtime->state=MODULE_LIVE;
    fail_open=1;assert(character_file_open(116u<<8,NULL,NULL,&handle)==-5 && !handle && closes==1 && !runtime->users);fail_open=0;
    assert(!character_file_open(116u<<8,NULL,NULL,&handle) && handle && runtime->users==1);
    assert(module_unload(owner.name,0)==-11);
    assert(character_device_unregister(&registration)==-16);
    assert(character_file_ioctl(handle,NULL,NULL,7,9)==16);
    assert(character_file_has_readv(handle));
    assert(character_file_readv(handle,NULL,NULL,NULL,0,true,31,7)==19);
    assert(character_file_read(handle,NULL,NULL,NULL,0)==-22);
    assert(character_file_write(handle,NULL,NULL,NULL,0)==-22);
    assert(character_file_poll(handle,NULL,1|4)==(1|4));
    character_file_close(handle,NULL);handle=NULL;
    assert(!runtime->users && closes==2);
    fail_after=0;assert(character_file_open(116u<<8,NULL,NULL,&handle)==-12 && !handle && !runtime->users);fail_after=-1;
    assert(character_file_open(117u<<8,NULL,NULL,&handle)==-19 && !handle);
    assert(character_file_open(UINT64_C(1)<<40,NULL,NULL,&handle)==-19 && !handle);
    struct character_device_registration invalid=registration;invalid.count=0;
    assert(character_device_register(&invalid)==-22);
    invalid=registration;invalid.first_minor=1048575;invalid.count=2;
    assert(character_device_register(&invalid)==-22);
    assert(opens==2 && closes==2);
    assert(!module_unload(owner.name,0) && !module_list && !owner.runtime && !allocated);
    assert(character_file_open(116u<<8,NULL,NULL,&handle)==-19 && !handle);
    character_file_close(NULL,NULL);
    for(int fail=0;fail<2;++fail) {
        runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
        strcpy(owner.name,"failed_chr");strcpy(runtime->name,owner.name);owner.runtime=runtime;runtime->owner=&owner;
        runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
        fail_after=fail;
        assert(character_device_register(&registration)==-12);fail_after=-1;
        runtime->state=MODULE_LIVE;
        assert(character_file_open(116u<<8,NULL,NULL,&handle)==-19 && !handle);
        assert(!module_unload(owner.name,0) && !allocated);
    }
    struct character_device_registration builtin=registration;builtin.owner=NULL;
    assert(!character_device_register(&builtin));
    assert(!character_device_unregister(&builtin) && !allocated);
    assert(!character_device_register(&builtin));
    assert(!character_file_open((116u<<8)|127u,NULL,NULL,&handle));
    assert(character_device_unregister(&builtin)==-16);
    character_file_close(handle,NULL);
    assert(!character_device_unregister(&builtin) && !allocated);
    const struct character_file_operations scalar_operations={0};
    builtin.operations=&scalar_operations;
    assert(!character_device_register(&builtin));
    assert(!character_file_open(116u<<8,NULL,NULL,&handle));
    assert(!character_file_has_readv(handle));
    assert(character_file_readv(handle,NULL,NULL,NULL,0,false,0,0)==-95);
    character_file_close(handle,NULL);
    assert(!character_device_unregister(&builtin) && !allocated);
    puts("PASS character registry: real module COMING/LIVE/unload, overlap/bounds, callback reentry, partial open cleanup, allocation rollback and final teardown");
}
