#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../kernel/module/internal.h"

static long fail_after = -1;
static size_t live_heap, live_maps;
void *kernel_malloc(size_t n)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *p = calloc(1, n);
    if (p) ++live_heap;
    return p;
}
void kernel_free(void *p) { if (p) { assert(live_heap); --live_heap; free(p); } }
int module_memory_alloc(size_t size, size_t alignment, struct module_memory **out)
{
    assert(alignment <= 4096);
    struct module_memory *m = kernel_malloc(sizeof(*m));
    if (!m) return -12;
    m->size = (size + 4095) & ~(size_t)4095;
    void *p = mmap(NULL, m->size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { kernel_free(m); return -12; }
    memset(p,0xa5,m->size); /* NOBITS must be materialized by the real layout code. */
    m->address = (uintptr_t)p;
    *out = m; ++live_maps;
    return 0;
}
int module_memory_protect(struct module_memory *m, unsigned int permissions)
{
    int p = PROT_READ;
    if (permissions & MODULE_MEMORY_WRITE) p |= PROT_WRITE;
    if (permissions & MODULE_MEMORY_EXEC) p |= PROT_EXEC;
    assert(!(p & PROT_WRITE) || !(p & PROT_EXEC));
    return mprotect((void *)m->address, m->size, p) ? -13 : 0;
}
void module_memory_free(struct module_memory *m)
{
    if (m) { assert(!munmap((void *)m->address,m->size)); --live_maps; kernel_free(m); }
}
static int imported_function(int a) { return a + 40; }
static int lookup(const char *name, uint64_t *value, void *context)
{
    (void)context;
    if (!strcmp(name, "imported_function")) { *value = (uintptr_t)imported_function; return 0; }
    return -2;
}
static void corrupt_rejected(const void *source, size_t size, size_t offset,
                             const void *value, size_t width)
{
    unsigned char *p = malloc(size);
    assert(p && offset <= size && width <= size-offset);
    memcpy(p,source,size); memcpy(p+offset,value,width);
    struct module_elf view;
    assert(module_elf_validate(p,size,&view) == -8);
    free(p);
}
/** @brief Check RELA values, signed boundaries and unchanged bytes on rejection.
 * Exercises the real relocator with independent literal x86-64 expectations.
 */
static void relocation_tests(void)
{
    unsigned char bytes[16];
    memset(bytes,0xcc,sizeof(bytes));
    assert(!module_apply_relocation(bytes,8,1,0xffffff9000001000ULL,-8,0));
    uint64_t u64; memcpy(&u64,bytes,8); assert(u64==0xffffff9000000ff8ULL);
    assert(!module_apply_relocation(bytes,4,2,0x1000,-4,0x1100));
    int32_t i32; memcpy(&i32,bytes,4); assert(i32==-260);
    assert(!module_apply_relocation(bytes,4,4,0x1000,0,0x1000));
    assert(!module_apply_relocation(bytes,4,10,UINT32_MAX,0,0));
    assert(module_apply_relocation(bytes,4,10,UINT32_MAX,1,0)==-8);
    assert(!module_apply_relocation(bytes,4,11,UINT64_MAX,0,0));
    assert(!module_apply_relocation(bytes,4,11,0,-1,0));
    memcpy(&i32,bytes,4);assert(i32==-1);
    assert(!module_apply_relocation(bytes,4,11,0,INT32_MIN,0));
    memcpy(&i32,bytes,4);assert(i32==INT32_MIN);
    unsigned char signed_original[16];memcpy(signed_original,bytes,16);
    assert(module_apply_relocation(bytes,4,11,0,(int64_t)INT32_MIN-1,0)==-8);
    assert(!memcmp(bytes,signed_original,16));
    assert(module_apply_relocation(bytes,4,11,0x80000000,0,0)==-8);
    assert(!module_apply_relocation(bytes,8,24,0x1000,0,0x1008));
    memcpy(&u64,bytes,8); assert(u64==UINT64_MAX-7);
    unsigned char original[16]; memcpy(original,bytes,16);
    assert(module_apply_relocation(bytes,8,1,UINT64_MAX,1,0)==-8);
    assert(module_apply_relocation(bytes,8,1,0,-1,0)==-8);
    assert(module_apply_relocation(bytes,3,2,0,0,0)==-8);
    assert(module_apply_relocation(bytes,8,2,0,0,UINT64_MAX)==-8);
    assert(module_apply_relocation(bytes,8,255,0,0,0)==-8);
    assert(!memcmp(bytes,original,16));
    assert(!module_apply_relocation(bytes,0,0,0,0,0));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    FILE *file=fopen(argv[1],"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long length=ftell(file); assert(length>0);
    rewind(file); unsigned char *image=malloc((size_t)length+1); assert(image);
    assert(fread(image,1,(size_t)length,file)==(size_t)length); fclose(file);
    struct module_elf view;
    assert(!module_elf_validate(image,(size_t)length,&view));
    for(size_t n=0;n<sizeof(struct module_elf_header);++n)
        assert(module_elf_validate(image,n,&view)==-8);
    assert(!module_elf_validate(image,(size_t)length,&view));
    uint64_t bad64=UINT64_MAX; uint16_t bad16=0xffff; uint8_t bad8=0;
    corrupt_rejected(image,length,0,&bad8,1);
    corrupt_rejected(image,length,40,&bad64,8); /* e_shoff */
    corrupt_rejected(image,length,62,&bad16,2); /* e_shstrndx */
    struct module_elf_section section;
    for(unsigned i=1;i<view.header.shnum;++i) {
        assert(!module_elf_get_section(&view,i,&section));
        size_t at=view.header.shoff+i*sizeof(section);
        corrupt_rejected(image,length,at,&bad16,2); /* sh_name */
        if(section.type!=8 && section.size)
            corrupt_rejected(image,length,at+24,&bad64,8);
        if(section.type==4) {
            uint32_t bad32=UINT32_MAX;
            corrupt_rejected(image,length,at+40,&bad32,4); /* sh_link */
            corrupt_rejected(image,length,at+44,&bad32,4); /* sh_info */
            if(section.size) {
                corrupt_rejected(image,length,section.offset,&bad64,8); /* r_offset */
                corrupt_rejected(image,length,section.offset+8,&bad64,8); /* r_info */
            }
        }
    }
    /* Unaligned input must not become a C-ABI alignment violation. */
    memmove(image+1,image,(size_t)length);
    assert(!module_elf_validate(image+1,(size_t)length,&view));
    for(long fail=0;;++fail) {
        struct module_layout layout={0}; fail_after=fail;
        int rc=module_layout_create(&view,&layout);
        if(!rc) {
            fail_after=-1;
            int found_bss=0;
            for(unsigned i=1;i<view.header.shnum;++i) {
                assert(!module_elf_get_section(&view,i,&section));
                if(section.type==8 && section.size>16000) {
                    assert(layout.sections[i].memory);
                    const unsigned char *data=(void *)layout.sections[i].memory->address;
                    for(size_t n=0;n<section.size;++n) assert(!data[n]);
                    found_bss=1;
                }
            }
            assert(found_bss);
            assert(!module_layout_relocate(&view,&layout,lookup,NULL));
            uint64_t entry=module_layout_find_symbol(&view,&layout,"fixture_entry");
            assert(entry);
            assert(!module_layout_seal(&layout));
            assert(((int (*)(int))(uintptr_t)entry)(2)==42);
            module_layout_destroy(&layout);
            assert(!live_heap && !live_maps);
            break;
        }
        assert(rc==-12); assert(!live_heap && !live_maps);
        assert(fail<1000);
    }
    fail_after=-1;
    struct module_layout layout={0}; assert(!module_layout_create(&view,&layout));
    assert(module_layout_relocate(&view,&layout,NULL,NULL)==-2);
    module_layout_destroy(&layout); assert(!live_heap && !live_maps);
    relocation_tests(); free(image);
    puts("PASS native ET_REL: real code, unaligned input, corruption, NOBITS, RELA, overflow, rollback");
}
