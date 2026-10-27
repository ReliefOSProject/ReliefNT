#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "../../block/core/storage_internal.h"
#include "../../fs/security/sidecar.h"

static uint32_t max_pages;
static int fail_alloc;

/** @brief Allocate a bounded host buffer as a stand-in for kernel page ownership. */
uint64_t mm_alloc_pages(uint32_t count)
{
    if (count > max_pages) max_pages = count;
    return fail_alloc ? 0 : (uintptr_t)calloc(count, 4096);
}
/** @brief Release only pages returned by this fixture's allocator. */
void mm_free_pages(uint64_t address, uint32_t count) { (void)count; free((void *)(uintptr_t)address); }
void *mm_phys_to_virt(uint64_t address) { return (void *)(uintptr_t)address; }
uint64_t mm_virt_to_phys(const void *address) { return (uintptr_t)address; }
/** @brief Provide the storage module's ordinary string/memory helpers. */
void storage_memzero(void *out, size_t count) { memset(out, 0, count); }
/** @brief Copy exactly the requested bytes. */
void storage_memcpy(void *out, const void *in, size_t count) { memcpy(out, in, count); }
/** @brief Return a borrowed string's length. */
size_t storage_strlen(const char *text) { return strlen(text); }
/** @brief Compare FAT names without case. */
int storage_text_eq_ci(const char *left, const char *right) { return !strcasecmp(left, right); }
/** @brief Compare ordinary absolute paths exactly. */
int storage_text_eq(const char *left, const char *right) { return !strcmp(left, right); }
/** @brief Bound a NUL-terminated path copy. */
void storage_copy_text(char *out, uint32_t size, const char *in) { snprintf(out, size, "%s", in); }

struct directory_image { unsigned char *data; size_t length; int error; };
static int bound_read(void *context,const void **data,size_t *length)
{
    struct directory_image *directory=context;
    if(directory->error)return directory->error;
    if(!directory->data)return -2;
    uint64_t address=mm_alloc_pages((directory->length+4095)/4096);
    if(!address)return -12;
    memcpy((void *)(uintptr_t)address,directory->data,directory->length);
    *data=(void *)(uintptr_t)address;*length=directory->length;return 0;
}
static int bound_write(void *context,const void *data,uint32_t length)
{
    struct directory_image *directory=context;
    if(directory->error)return directory->error;
    unsigned char *next=malloc(length);assert(next);memcpy(next,data,length);
    free(directory->data);directory->data=next;directory->length=length;return 0;
}
static void bound_directories(void)
{
    struct directory_image a={0},b={0};
    struct storage_sidecar_io first={&a,bound_read,bound_write};
    struct storage_sidecar_io second={&b,bound_read,bound_write};
    struct reliefos_permissions value={0601,101,202},other={0640,303,404},got;
    assert(!storage_sidecar_permissions_at(&first,"same","/etc/same",&value,true));
    assert(!storage_sidecar_permissions_at(&second,"same","/else/same",&other,true));
    assert(!storage_sidecar_permissions_at(&first,"SAME","/moved/same",&got,false));
    assert(!memcmp(&got,&value,sizeof(got)));
    assert(!storage_sidecar_permissions_at(&second,"same","/moved/same",&got,false));
    assert(!memcmp(&got,&other,sizeof(got)));
    assert(!storage_sidecar_permissions_at(&first,"shadow","/etc/shadow",&got,false));
    assert(got.mode==0600 && !got.uid && !got.gid);
    a.error=-11;
    assert(storage_sidecar_permissions_at(&first,"same","/etc/same",&got,false)==-11);
    assert(storage_sidecar_permissions_at(&first,"same","/etc/same",&other,true)==-11);
    a.error=0;a.data[12]^=1;
    assert(storage_sidecar_permissions_at(&first,"same","/etc/same",&got,false)==-5);
    assert(storage_sidecar_permissions_at(&first,"../same","/etc/same",&value,true)==-22);
    assert(storage_sidecar_permissions_at(&first,"a/b","/etc/same",&value,true)==-22);
    free(a.data);free(b.data);
    puts("PASS bound sidecar directories: independent owners, moved names, private shadow default, EAGAIN and corruption; no namespace I/O");
}

/** @brief Preserve more than one GRUB directory's worth of mode/owner records. */
int main(void)
{
    bound_directories();
    struct directory_image directory={0};
    struct storage_sidecar_io io={&directory,bound_read,bound_write};
    for(unsigned i=0;i<600;++i) {
        char name[64];snprintf(name,sizeof(name),"module-%04u.mod",i);
        struct reliefos_permissions value={0640,1000+i,2000+i};
        assert(!storage_sidecar_permissions_at(&io,name,"/boot/grub/i386-pc/module.mod",&value,true));
    }
    for(unsigned i=0;i<600;++i) {
        char name[64],backup[68];snprintf(name,sizeof(name),"module-%04u.mod",i);
        snprintf(backup,sizeof(backup),"%s~",name);
        struct reliefos_permissions old,fresh={0600,0,0};
        struct storage_sidecar_change change={0};
        assert(!storage_sidecar_prepare_rename(&io,name,backup,&fresh,&change));
        assert(!bound_write(&directory,change.after,change.length));storage_sidecar_change_release(&change);
        assert(!storage_sidecar_permissions_at(&io,backup,"/boot/grub/i386-pc/module.mod",&old,false));
        assert(old.uid==1000+i && old.gid==2000+i && old.mode==0640);
        assert(!storage_sidecar_permissions_at(&io,name,"/boot/grub/i386-pc/module.mod",&fresh,true));
    }
    assert(directory.length>8192 && max_pages<64);
    unsigned char *before=malloc(directory.length);assert(before);memcpy(before,directory.data,directory.length);
    fail_alloc=1;struct reliefos_permissions value={0777,99,99};
    assert(storage_sidecar_permissions_at(&io,"new.mod","/boot/grub/i386-pc/new.mod",&value,true)==-12);
    assert(!memcmp(before,directory.data,directory.length));fail_alloc=0;free(before);
    directory.data[12]^=1;
    assert(storage_sidecar_permissions_at(&io,"module-0000.mod","/boot/grub/i386-pc/module.mod",&value,false)==-5);
    free(directory.data);
    puts("PASS FAT metadata: 1200 mode/owner records, GRUB backup renames, bounded allocation and corruption/failure refusal");
    return 0;
}
