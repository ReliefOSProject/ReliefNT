/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: fb blit.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/syscall/device/legacy_ioctl.c"
static struct task current;
static struct task_file original, replacement;
static struct framebuffer screen={.width=10000,.height=2,.pitch=40000,.bpp=32,.bytes_per_pixel=4,.available=true};
static struct task_vma vmas[2];
static uint64_t blocked,resident,stack_base,stack_end,tmpfs_size;
static bool contended,fail_stat;
static unsigned allocations,preparations,blits,presents,pte_checks;
void *kernel_malloc(size_t n){void *p=malloc(n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
struct task *sched_current_task(void){return &current;}
struct task_file *task_file_for_io(struct task *task,int fd)
{assert(task==&current && fd==3);return &original;}
struct task_file *task_file_for_fd(struct task *task,int fd)
{(void)task;(void)fd;return &replacement;}
int task_evdev_ioctl(struct task_file *file,uint64_t request,uint64_t argument)
{(void)file;(void)request;(void)argument;return -RELIEFOS_ENOTTY;}
int task_block_device(const struct task_file *file,uint32_t *disk,int32_t *partition)
{(void)file;(void)disk;(void)partition;return 0;}
const struct framebuffer *framebuffer_get(void){return &screen;}
uint32_t pty_vt_number(uint32_t id){return id;}
uint32_t pty_vt_active(void){return 1;}
int pty_vt_graphical_active(void){return 1;}
bool user_range_ok(uint64_t address,uint64_t size){return address>=4096 && size<=UINT64_MAX-address;}
bool user_range_writable(uint64_t address,uint64_t size){return user_range_ok(address,size);}
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{assert(task==&current && size<=32768);(void)writing;++preparations;
 return contended && address==blocked ? SWAP_STORAGE_CONTENDED : 0;}
uint32_t sched_task_vma_capacity(const struct task *task){assert(task==&current);return 2;}
struct task_vma *sched_task_vma_at(struct task *task,uint32_t i){assert(task==&current && i<2);return &vmas[i];}
bool address_space_user_page_readable(const struct address_space *as,uint64_t page)
{assert(as==sched_task_as(&current));++pte_checks;return page==resident;}
enum paging_user_entry_kind paging_user_entry_kind(const struct address_space *as,uint64_t page)
{assert(as==sched_task_as(&current));(void)page;return PAGING_USER_ENTRY_EMPTY;}
bool mm_user_stack_readable_extent(struct task *task,uint64_t page,uint64_t *end)
{assert(task==&current);if(page<stack_base || page>=stack_end)return false;*end=stack_end;return true;}
int storage_inode_stat(const struct storage_node *node,struct linux_stat_abi *out)
{assert(node->flags&STORAGE_NODE_FLAG_TMPFS);if(fail_stat)return -RELIEFOS_EIO;
 *out=(struct linux_stat_abi){.st_size=tmpfs_size};return 0;}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *task){(void)q;(void)task;assert(0);}
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;assert(0);}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;assert(0);return 0;}
void task_process_vm_scope_release(void *state){(void)state;assert(0);}
void framebuffer_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t color)
{(void)x;(void)y;(void)w;(void)h;(void)color;assert(0);}
void framebuffer_present_region(uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{assert(!x && !y && w==10000 && h==2);++presents;}
void framebuffer_blit(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t stride,const uint32_t *data)
{assert(!x && !y && w==10000 && h==2 && stride==w);
 for(uint32_t row=0;row<h;++row)for(uint32_t col=0;col<w;++col)
     assert(data[row*w+col]==((row*12000u+col)^0x51842u));++blits;}
static int64_t call_blit(struct reliefos_fb_present *request)
{struct trap_frame frame={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=RELIEFOS_FBIOBLIT,.rdx=(uintptr_t)request};
 task_lookup_scope_begin(&current,&frame);
 return syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,RELIEFOS_FBIOBLIT,(uintptr_t)request,0,0,0);}
static int64_t call_cmap(uint64_t command,struct fb_cmap *request)
{struct trap_frame frame={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=command,.rdx=(uintptr_t)request};
 task_lookup_scope_begin(&current,&frame);
 return syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,command,(uintptr_t)request,0,0,0);}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=1;current.controlling_pty_id=1;
    original=(struct task_file){.used=1,.flags=TASK_FILE_FLAG_DEV_NODE,
        .node={.first_cluster=STORAGE_DEV_KIND_FB0}};
    replacement=(struct task_file){.used=1};current.syscall_file=&original;current.syscall_fd=3;
    uint32_t *pixels=aligned_alloc(4096,98304);assert(pixels);
    for(uint32_t i=0;i<24576;++i)pixels[i]=i^0x51842u;
    uint64_t base=(uintptr_t)pixels;
    vmas[0]=(struct task_vma){.used=1,.start=base,.end=base+98304,.flags=TASK_VMA_FLAG_ANON,.prot=TASK_VMA_PROT_WRITE};
    struct reliefos_fb_present request={.width=10000,.height=2,.stride=12000,.pixels=base};
    blocked=base+32768;contended=true;
    assert(call_blit(&request)==KERNEL_SYSCALL_SWAP_CONTENDED && !blits && !presents);
    memset(pixels,0,32768);vmas[0].start=base+32768;contended=false;
    assert(!call_blit(&request) && blits==1 && presents==1);
    task_lookup_scope_cancel(&current);assert(!allocations);
    vmas[0].start=base;
    for(uint32_t i=0;i<24576;++i)pixels[i]=i^0x51842u;
    contended=true;assert(call_blit(&request)==KERNEL_SYSCALL_SWAP_CONTENDED);
    screen.width=9999;contended=false;
    assert(call_blit(&request)==-RELIEFOS_EAGAIN && blits==1 && presents==1);
    task_lookup_scope_cancel(&current);assert(!allocations);screen.width=10000;

    uint16_t red[2]={11,12},green[2]={21,22},blue[2]={31,32},same[2]={0};
    struct fb_cmap cmap={.len=2,.red=(uintptr_t)red,.green=(uintptr_t)green,.blue=(uintptr_t)blue};
    blocked=(uintptr_t)green;contended=true;
    assert(call_cmap(FBIOPUTCMAP,&cmap)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!reliefos_fb_palette[0][0]);red[0]=99;contended=false;
    assert(!call_cmap(FBIOPUTCMAP,&cmap));
    assert(reliefos_fb_palette[0][0]==11 && reliefos_fb_palette[1][0]==21);
    task_lookup_scope_cancel(&current);assert(!allocations);
    uint64_t lo=(uintptr_t)red,hi=(uintptr_t)red+sizeof(red);
    const uint64_t starts[]={(uintptr_t)green,(uintptr_t)blue,(uintptr_t)same};
    for(unsigned i=0;i<3;++i){if(starts[i]<lo)lo=starts[i];if(starts[i]+4>hi)hi=starts[i]+4;}
    vmas[0]=(struct task_vma){.used=1,.start=lo&~4095ULL,.end=(hi+4095)&~4095ULL,
        .flags=TASK_VMA_FLAG_ANON,.prot=TASK_VMA_PROT_WRITE};
    memset(red,0,sizeof(red));memset(green,0,sizeof(green));memset(blue,0,sizeof(blue));
    blocked=(uintptr_t)green;contended=true;
    assert(call_cmap(FBIOGETCMAP,&cmap)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(red[0]==11);reliefos_fb_palette[1][0]=100;contended=false;
    assert(!call_cmap(FBIOGETCMAP,&cmap) && green[0]==21);
    task_lookup_scope_cancel(&current);assert(!allocations);
    cmap.red=cmap.green=cmap.blue=(uintptr_t)same;
    assert(!call_cmap(FBIOGETCMAP,&cmap) && same[0]==31 && same[1]==32);
    task_lookup_scope_cancel(&current);assert(!allocations);

    vmas[0]=(struct task_vma){.used=1,.start=base,.end=base+4096,.flags=TASK_VMA_FLAG_ANON,.prot=TASK_VMA_PROT_WRITE};
    vmas[0].end=base+4096;pte_checks=0;
    assert(!fb_readable_span(&current,base,1ULL<<40) && pte_checks<4);
    vmas[0].end=base+8192;vmas[0].flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_MMAP;
    vmas[0].file_node.flags=STORAGE_NODE_FLAG_TMPFS;tmpfs_size=4097;
    assert(fb_readable_span(&current,base,8192));
    vmas[0].end=base+12288;assert(!fb_readable_span(&current,base,12288));
    fail_stat=true;resident=base;
    assert(fb_readable_span(&current,base,4096));
    assert(!fb_readable_span(&current,base+4096,4096));
    fail_stat=false;resident=0;
    vmas[0].used=0;resident=base;assert(fb_readable_span(&current,base,4096));
    resident=0;stack_base=base;stack_end=base+8192;
    assert(fb_readable_span(&current,base,8192));
    stack_base=stack_end=0;free(pixels);
    puts("PASS fb blit: staged rows, fd pin, geometry, sparse gaps, tmpfs EOF and stack eligibility");
    return 0;
}
