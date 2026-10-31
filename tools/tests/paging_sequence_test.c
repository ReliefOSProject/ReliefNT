/* Draft for tools/tests/. Calls the real paging sequence and CR3 consumers. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/paging.h>

#define WRITERS 8u
#define UPDATES 500000u
static pthread_barrier_t start;
static atomic_uint completed, reloads;
static atomic_bool reader_saw_regression;
static atomic_bool update_during_reload;

uint32_t smp_current_cpu(void) { return 0; }
void x86_64_load_cr3(uint64_t value)
{
    assert(value==0x1000); atomic_fetch_add(&reloads,1);
    if (atomic_exchange(&update_during_reload,false)) paging_note_mm_change();
}

static void *writer(void *unused)
{
    (void)unused;
    int status=pthread_barrier_wait(&start);
    assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i=0;i<UPDATES;++i) paging_note_mm_change();
    atomic_fetch_add(&completed,1); return NULL;
}
static void *reader(void *unused)
{
    (void)unused;
    int status=pthread_barrier_wait(&start);
    assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t previous=paging_mm_change_seq();
    while (atomic_load(&completed)<WRITERS) {
        uint64_t current=paging_mm_change_seq();
        if (current<previous) atomic_store(&reader_saw_regression,true);
        previous=current;
    }
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"reload-race")) {
        /* An update after the CPU starts reloading must remain visible to the
         * next check; recording a newer sequence after the reload loses it. */
        atomic_store(&update_during_reload,true);
        paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==1);
        paging_load_cr3_if_changed(0x1000);
        if (atomic_load(&reloads)!=2)
            fputs("FAIL CR3 reload recorded a mapping update that happened during the load\n",stderr);
        assert(atomic_load(&reloads)==2);
        paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==2);
        puts("PASS real conditional CR3 reader retains changes during reload"); return 0;
    }
    assert(argc==1);
    /* Exercise real conditional-CR3 reader before and after a real update. */
    paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==1);
    paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==1);
    paging_note_mm_change(); paging_load_cr3_if_changed(0x1000);
    assert(atomic_load(&reloads)==2);
    const uint64_t before=paging_mm_change_seq();
    pthread_t writers[WRITERS], observer;
    assert(!pthread_barrier_init(&start,NULL,WRITERS+1));
    for (unsigned i=0;i<WRITERS;++i) assert(!pthread_create(&writers[i],NULL,writer,NULL));
    assert(!pthread_create(&observer,NULL,reader,NULL));
    for (unsigned i=0;i<WRITERS;++i) assert(!pthread_join(writers[i],NULL));
    assert(!pthread_join(observer,NULL)); assert(!pthread_barrier_destroy(&start));
    uint64_t after=paging_mm_change_seq();
    if (after-before!=(uint64_t)WRITERS*UPDATES)
        fprintf(stderr,"FAIL paging sequence lost concurrent updates: expected %u, got %llu\n",
                WRITERS*UPDATES,(unsigned long long)(after-before));
    assert(after-before==(uint64_t)WRITERS*UPDATES && !atomic_load(&reader_saw_regression));
    paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==3);
    paging_load_cr3_if_changed(0x1000); assert(atomic_load(&reloads)==3);
    puts("PASS real paging sequence: concurrent updates, reader monotonicity and conditional CR3 reload");
}
