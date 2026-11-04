/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../kernel/syscall/internal.h"
#include "../../kernel/sync/lock.c"
#include "../../kernel/sync/wait.c"

static void *observed_copy(void *destination, const void *source, size_t length);
#define __builtin_memcpy observed_copy
#include "../../kernel/syscall/ipc/ipc.c"
#undef __builtin_memcpy

static struct task_pipe pipes[2];
static unsigned char rings[2][TASK_PIPE_RING_CAP];
static struct task_file ends[2][2];
static struct kernel_object_table objects;
static _Thread_local struct task owner;
static _Thread_local unsigned cpu;
static _Thread_local uint64_t irq;
static _Thread_local bool resident = true, writable = true, resume_ok = true;
static char syscall_trace_prefix[128];
static pthread_mutex_t rendezvous = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static const void *held_source;
static bool copying, independent_finished;

uint64_t kernel_irq_save(void) { uint64_t saved=irq;irq=0;return saved; }
void kernel_irq_restore(uint64_t flags) { irq=flags; }
uint32_t smp_current_cpu(void) { return cpu; }
void smp_membarrier_poll(void) { }
struct task *sched_current_task(void) { return &owner; }
uint32_t sched_current_pid(void) { return owner.pid; }
void sched_sleep_current_until(uint64_t tick) { (void)tick;owner.state=TASK_BLOCKED; }
void sched_wake_interruptible(struct task *task)
{ if(task->state==TASK_BLOCKED)task->state=TASK_READY; }
int sched_signal_user_task(uint32_t pid,int signal) { (void)pid;(void)signal;abort(); }
struct kernel_object_table *kernel_objects(void) { return &objects; }
void *kernel_object_lookup(struct kernel_object_table *table,uint32_t handle,enum kernel_object_type type)
{ assert(table==&objects && type==KERNEL_OBJECT_PIPE);return handle>=1 && handle<=2 ? &pipes[handle-1] : NULL; }
void kernel_free(void *pointer) { free(pointer); }
struct task_file *task_file_for_fd(struct task *task,int fd)
{ assert(task==&owner && execution_readers && !irq);return fd>=3 && fd<7 ? &ends[(fd-3)/2][(fd-3)%2] : NULL; }
bool address_space_user_page_copyable(const struct address_space *as,uint64_t page,bool writing)
{ (void)as;(void)page;assert(__atomic_load_n(&execution_readers,__ATOMIC_ACQUIRE) && !irq);return resident && (!writing || writable); }
bool sched_try_resume_current_user_frame(const struct trap_frame *frame)
{ assert(__atomic_load_n(&execution_readers,__ATOMIC_ACQUIRE) && !irq);if(resume_ok)owner.frame=*frame;return resume_ok; }
void arch_set_user_fs(uint64_t base) { assert(__atomic_load_n(&execution_readers,__ATOMIC_ACQUIRE) && base==owner.fs_base); }

#include "pipe_entry.inc"

/** @brief Initialize one host thread as a distinct logical kernel CPU. */
static void current(unsigned index)
{
    cpu=index;irq=0;resident=writable=resume_ok=true;
    owner=(struct task){.pid=100+index,.kind=TASK_KIND_USER,.state=TASK_RUNNING,.running_cpu=index};
    strcpy(owner.path,"/usr/bin/pipe-probe");
}

/** @brief Reset unpublished pipes after every previous worker has joined. */
static void reset(void)
{
    memset(pipes,0,sizeof(pipes));
    for(unsigned i=0;i<2;++i) {
        pipes[i]=(struct task_pipe){.used=1,.readers=1,.writers=1,.handle=i+1,
            .capacity=TASK_PIPE_CAP,.data=rings[i]};
        kernel_wait_queue_init(&pipes[i].wait_read);kernel_wait_queue_init(&pipes[i].wait_write);
        ends[i][0]=(struct task_file){.used=1,.flags=TASK_FILE_FLAG_PIPE|RELIEFOS_O_RDONLY,.aux=i+1};
        ends[i][1]=(struct task_file){.used=1,.flags=TASK_FILE_FLAG_PIPE|TASK_FILE_FLAG_PIPE_WRITE|RELIEFOS_O_WRONLY,.aux=i+1};
    }
    current(0);
}

/** @brief Invoke actual syscall admission, keeping rejected frames unchanged. */
static bool invoke(bool writing,unsigned pipe,void *buffer,size_t length,int64_t *result)
{
    struct trap_frame frame={.rax=writing ? LINUX_SYS_WRITE : LINUX_SYS_READ,
        .rdi=3+2*pipe+writing,.rsi=(uintptr_t)buffer,.rdx=length};
    struct trap_frame before=frame;struct task *resume=NULL;
    bool done=syscall_try_pipe_io(&frame,&resume);
    assert(!irq);
    if(done) { *result=(int64_t)frame.rax;assert(resume==(resume_ok ? &owner : NULL)); }
    else assert(!memcmp(&before,&frame,sizeof(frame)) && !resume);
    return done;
}

/** @brief Hold a real ring copy to expose unintended cross-pipe serialization.
 * @return memcpy's destination; instrumentation never changes copied bytes. */
static void *observed_copy(void *destination,const void *source,size_t length)
{
    if(source==held_source) {
        assert(!pthread_mutex_lock(&rendezvous));copying=true;
        assert(!pthread_cond_broadcast(&changed));
        struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=3;
        while(!independent_finished)assert(!pthread_cond_timedwait(&changed,&rendezvous,&deadline));
        assert(!pthread_mutex_unlock(&rendezvous));
    }
    return memcpy(destination,source,length);
}

struct operation { unsigned cpu,pipe; unsigned char *bytes; bool hold; };
/** @brief Complete one pipe write while an independent pipe's copy is held. */
static void *independent(void *opaque)
{
    struct operation *operation=opaque;current(operation->cpu);int64_t result=-1;
    if(!operation->hold) {
        assert(!pthread_mutex_lock(&rendezvous));
        while(!copying)assert(!pthread_cond_wait(&changed,&rendezvous));
        assert(!pthread_mutex_unlock(&rendezvous));
    }
    assert(invoke(true,operation->pipe,operation->bytes,64,&result) && result==64);
    if(!operation->hold) {
        assert(!pthread_mutex_lock(&rendezvous));independent_finished=true;
        assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));
    }
    return NULL;
}

/** @brief Publish PIPE_BUF-sized records from competing writers on one pipe. */
static void *contending(void *opaque)
{
    unsigned index=(uintptr_t)opaque;current(index);
    unsigned char record[64];memset(record,(int)index,sizeof(record));
    for(unsigned i=0;i<2000;++i) {
        int64_t result;
        if(!invoke(true,0,record,sizeof(record),&result)) { --i;continue; }
        assert(result==sizeof(record));
    }
    return NULL;
}

int main(void)
{
    reset();unsigned char bytes[65536],out[65536];memset(bytes,0x5a,sizeof(bytes));int64_t result=-1;
    if(!invoke(true,0,bytes,64,&result)) {
        fputs("FAIL: ready pipe syscall did not complete under shared execution ownership\n",stderr);return 1;
    }
    assert(result==64 && owner.proc_write_calls==1 && owner.proc_write_chars==64);
    assert(invoke(false,0,out,64,&result) && result==64 && !memcmp(bytes,out,64));
    assert(owner.proc_read_calls==1 && owner.proc_read_chars==64);
    assert(!invoke(false,0,out,64,&result)); /* Empty blocking read keeps its full wait path. */
    resident=false;assert(!invoke(true,0,bytes,64,&result));resident=true;
    owner.shared_mm=(void *)1;assert(!invoke(true,0,bytes,64,&result));owner.shared_mm=NULL;
    ends[0][1].flags|=TASK_FILE_FLAG_PATH;assert(!invoke(true,0,bytes,64,&result));ends[0][1].flags&=~TASK_FILE_FLAG_PATH;
    strcpy(syscall_trace_prefix,"/usr/bin/");assert(!invoke(true,0,bytes,64,&result));syscall_trace_prefix[0]=0;
    assert(invoke(true,0,bytes,65536,&result) && result==32768);
    writable=false;assert(!invoke(false,0,out,64,&result));writable=true;
    resume_ok=false;assert(invoke(false,0,out,32768,&result) && result==32768);resume_ok=true;
    owner.restart_syscall=1;assert(!invoke(true,0,bytes,64,&result));owner.restart_syscall=0;
    assert(invoke(true,0,NULL,0,&result) && result==0);
    assert(!invoke(true,0,NULL,1,&result));
    assert(!invoke(true,0,(void *)(RELIEFNT_USER_TOP-1),2,&result));
    ends[0][1].flags=TASK_FILE_FLAG_PIPE|RELIEFOS_O_RDONLY;
    assert(!invoke(true,0,bytes,64,&result));
    ends[0][1].flags=TASK_FILE_FLAG_PIPE|TASK_FILE_FLAG_PIPE_WRITE|RELIEFOS_O_WRONLY;
    pipes[0].readers=0;assert(!invoke(true,0,bytes,64,&result));pipes[0].readers=1;
    pipes[0].writers=0;assert(invoke(false,0,out,64,&result) && result==0);pipes[0].writers=1;
    pipes[0].named=true;assert(!invoke(true,0,bytes,64,&result));pipes[0].named=false;
    ends[0][1].path_ref.mount=(void *)1;assert(!invoke(true,0,bytes,64,&result));
    ends[0][1].path_ref.mount=NULL;
    assert(invoke(true,0,bytes,32768,&result) && result==32768);
    assert(invoke(true,0,bytes,32768,&result) && result==32768);
    assert(!invoke(true,0,bytes,64,&result));
    assert(invoke(false,0,out,1,&result) && result==1);
    assert(!invoke(true,0,bytes,64,&result)); /* No partial atomic record. */
    ends[0][1].flags|=RELIEFOS_O_NONBLOCK;
    assert(!invoke(true,0,bytes,64,&result));
    assert(invoke(true,0,bytes,8192,&result) && result==1); /* Large nonblocking short write. */
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    assert(!invoke(true,0,bytes,64,&result));kernel_execution_unlock_irqrestore(flags);

    reset();unsigned char a[64],b[64];memset(a,0xa5,64);memset(b,0x5a,64);
    held_source=a;pthread_t first,second;
    struct operation operations[]={{1,0,a,true},{2,1,b,false}};
    assert(!pthread_create(&first,NULL,independent,&operations[0]));
    assert(!pthread_create(&second,NULL,independent,&operations[1]));
    assert(!pthread_join(first,NULL) && !pthread_join(second,NULL));held_source=NULL;
    assert(invoke(false,0,out,64,&result) && result==64 && !memcmp(a,out,64));
    assert(invoke(false,1,out,64,&result) && result==64 && !memcmp(b,out,64));

    reset();pthread_t writers[2];assert(!pthread_create(&writers[0],NULL,contending,(void *)1));
    assert(!pthread_create(&writers[1],NULL,contending,(void *)2));
    unsigned counts[3]={0};
    for(unsigned i=0;i<4000;) {
        if(!invoke(false,0,out,64,&result))continue;
        assert(result==64 && (out[0]==1 || out[0]==2));
        for(unsigned j=1;j<64;++j)assert(out[j]==out[0]);
        ++counts[out[0]];++i;
    }
    assert(!pthread_join(writers[0],NULL) && !pthread_join(writers[1],NULL));
    assert(counts[1]==2000 && counts[2]==2000);
    assert(!__atomic_load_n(&execution_readers,__ATOMIC_ACQUIRE));
    puts("PASS independent pipe progress, competing writer records, resident admission, writer exclusion, counters and fallbacks");
    return 0;
}
