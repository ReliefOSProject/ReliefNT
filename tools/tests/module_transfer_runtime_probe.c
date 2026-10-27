/* SPDX-License-Identifier: Apache-2.0 */
/* Selected PID1 only: original OFDs and buffered bytes must outlive fd reuse. */
#define main module_previous_probe
#include "module_runtime_probe.c"
#undef main
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/sendfile.h>

struct transfer_fixture {
    int source, replacement, target, output, reader;
    _Atomic int started;
    _Atomic off_t offset;
    unsigned char expected[4096];
};
_Static_assert(sizeof(_Atomic off_t)==sizeof(off_t),"native atomic off_t ABI");

static void read_exact(int fd,void *bytes,size_t size)
{
    size_t done=0;
    while(done<size) {
        struct pollfd waiting={.fd=fd,.events=POLLIN};
        require(poll(&waiting,1,5000)==1 && (waiting.revents&POLLIN),"bounded transfer pipe receive");
        ssize_t count=read(fd,(char *)bytes+done,size-done);
        require(count>0,"read exact transfer payload");done+=(size_t)count;
    }
}
static void await_blocked_init(void)
{
    struct timespec start,now,delay={.tv_nsec=1000000};
    require(!clock_gettime(CLOCK_MONOTONIC,&start),"start transfer wait deadline");
    for(;;) {
        int fd=open("/proc/1/stat",O_RDONLY);char record[1024]={0};
        require(fd>=0,"inspect selected init state");
        ssize_t count=read(fd,record,sizeof(record)-1);
        require(count>0 && !close(fd),"read selected init state");
        char *end=strrchr(record,')');
        require(end && end[1]==' ',"parse selected init state");
        if(end[2]=='S')return;
        require(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5,
            "wait for actual blocked sendfile before descriptor replacement");
        nanosleep(&delay,NULL);
    }
}
static void *replace_transfer_descriptors(void *argument)
{
    struct transfer_fixture *fixture=argument;
    while(!atomic_load_explicit(&fixture->started,memory_order_acquire))sched_yield();
    await_blocked_init();
    require(dup2(fixture->replacement,fixture->source)==fixture->source &&
        dup2(fixture->target,fixture->output)==fixture->output,"replace both blocked transfer descriptors");
    atomic_store(&fixture->offset,99);
    unsigned char bytes[4096];read_exact(fixture->reader,bytes,sizeof(bytes));
    for(size_t i=0;i<sizeof(bytes);++i)require(bytes[i]==0x55,"drain exact preexisting pipe contents");
    read_exact(fixture->reader,bytes,sizeof(bytes));
    require(!memcmp(bytes,fixture->expected,sizeof(bytes)),"blocked sendfile retains original buffered data");
    return NULL;
}
static void transfer_description_probe(void)
{
    require(!mkdir("/dev/shm/ko-transfer-qa",0700),"reserve private transfer fixture");
    int directory=open("/dev/shm/ko-transfer-qa",O_RDONLY|O_DIRECTORY);
    require(directory>=0,"hold private transfer directory");
    struct transfer_fixture fixture={0};
    fixture.source=openat(directory,"source",O_CREAT|O_EXCL|O_RDWR,0600);
    fixture.replacement=openat(directory,"replacement",O_CREAT|O_EXCL|O_RDWR,0600);
    fixture.target=openat(directory,"target",O_CREAT|O_EXCL|O_RDWR,0600);
    require(fixture.source>=0 && fixture.replacement>=0 && fixture.target>=0,"create private transfer inodes");
    unsigned char changed[4096];memset(changed,0xdd,sizeof(changed));
    for(size_t i=0;i<sizeof(fixture.expected);++i)fixture.expected[i]=(unsigned char)(i*17+3);
    require(write(fixture.source,fixture.expected,sizeof(fixture.expected))==(ssize_t)sizeof(fixture.expected) &&
        write(fixture.replacement,changed,sizeof(changed))==(ssize_t)sizeof(changed),"populate distinguishable source inodes");
    off_t from=7,to=0;
    require(copy_file_range(fixture.source,&from,fixture.target,&to,17,0)==17 && from==24 && to==17,
        "copy_file_range publishes explicit source and destination offsets");
    unsigned char copied[17];
    require(pread(fixture.target,copied,sizeof(copied),0)==(ssize_t)sizeof(copied) &&
        !memcmp(copied,fixture.expected+7,sizeof(copied)) && lseek(fixture.source,0,SEEK_CUR)==4096 &&
        lseek(fixture.target,0,SEEK_CUR)==0,"range copy uses original provider and preserves OFD offsets");
    require(!ftruncate(fixture.target,0) && !unlinkat(directory,"source",0),"unlink original transfer inode while held");
    int channel[2];require(!pipe(channel),"create blocked transfer channel");
    fixture.reader=channel[0];fixture.output=channel[1];
    require(fcntl(fixture.output,F_SETPIPE_SZ,4096)==4096,"select one atomic pipe payload capacity");
    int writer=dup(fixture.output);require(writer>=0,"retain pipe endpoint during descriptor replacement");
    unsigned char padding[4096];memset(padding,0x55,sizeof(padding));
    require(write(fixture.output,padding,sizeof(padding))==(ssize_t)sizeof(padding),"fill pipe before sendfile wait");
    pthread_t worker;
    require(!pthread_create(&worker,NULL,replace_transfer_descriptors,&fixture),"start shared-fd transfer observer");
    atomic_store_explicit(&fixture.started,1,memory_order_release);
    ssize_t sent=sendfile(fixture.output,fixture.source,(off_t *)(void *)&fixture.offset,sizeof(fixture.expected));
    require(sent==(ssize_t)sizeof(fixture.expected),"blocked sendfile completes original invocation");
    require(!pthread_join(worker,NULL) && atomic_load(&fixture.offset)==4096,"accepted offset uses original snapshot after user input changes");
    struct stat status;
    require(!fstat(fixture.target,&status) && !status.st_size,"reused output descriptor receives no transfer data");
    require(!close(writer) && !close(fixture.reader) && !close(fixture.output) && !close(fixture.source) &&
        !close(fixture.replacement) && !close(fixture.target),"release shared transfer descriptions");
    require(!unlinkat(directory,"replacement",0) && !unlinkat(directory,"target",0) && !close(directory) &&
        !rmdir("/dev/shm/ko-transfer-qa"),"remove only private transfer fixture");
    puts("MODULE_RUNTIME_TRANSFER actual provider range copy, unlinked source, parked pipe, shared-fd reuse and immutable buffered bytes/offsets");
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only transfer probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);transfer_description_probe();return module_previous_probe();
}
