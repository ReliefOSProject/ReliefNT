#include "../internal.h"
/* Standalone ioctl fixtures may omit the anonymous inotify provider. */
extern int64_t task_inotify_fionread(struct task *, const struct task_file *, uint64_t) __attribute__((weak));
#include <reliefnt/swap.h>
#include <reliefnt/net_ipv6.h>
#include <linux/if.h>
#include <linux/socket.h>

/** @brief Prepare one actual fixed ioctl operand before device side effects.
 * @param address User address. @param size Exact bounded length.
 * @param writing True for output. @param fault Original command error.
 * @return Zero, the original error or a private prerequisite-wait marker.
 */
static int legacy_ioctl_prepare(uint64_t address, uint64_t size, bool writing, int fault)
{
#ifdef CONFIG_SWAP
    int rc = swap_prepare_user_range(sched_current_task(), address, size, writing);
    if (rc == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (rc < 0) return fault;
#else
    (void)address; (void)size; (void)writing; (void)fault;
#endif
    return 0;
}

#define IOCTL_PREPARE(address, size, writing, fault) do { \
    int result_ = legacy_ioctl_prepare((address), (size), (writing), (fault)); \
    if (result_) return result_; \
} while (0)


static uint16_t reliefos_fb_palette[4][256];
static uint32_t reliefos_fb_blank;

#ifdef CONFIG_SWAP
struct block_size_receipt {
    uint64_t address,request,value;
    uint32_t disk;
    int32_t partition;
    bool complete;
};

/** @brief Retain completed block geometry while preparing its user output.
 * @param task Current syscall owner. @param disk Original OFD's disk identity.
 * @param partition Original OFD's partition selector. @param request Size ABI.
 * @param address Original output address. @return Zero, errno or private wait.
 * Pending provider callbacks resume before any new page-in; after completion
 * only this task's completed storage owner is released. The lookup scope owns
 * the result through retries and common cancellation, without a user alias.
 */
static int block_ioctl_size(struct task *task,uint32_t disk,int32_t partition,
                            uint64_t request,uint64_t address)
{
    struct task_lookup_path *slot=NULL;int rc=task_lookup_scope_next(task,&slot);
    if(rc)return rc;
    struct block_size_receipt temporary={0};
    struct block_size_receipt *receipt=slot ? (void *)slot->name : NULL;
    if(receipt) {
        if(slot->flags!=TASK_LOOKUP_BLOCK_SIZE_KIND || receipt->address!=address ||
           receipt->request!=request || receipt->disk!=disk || receipt->partition!=partition)
            return -RELIEFOS_EINVAL;
    } else {
        if(!address)return -RELIEFOS_EFAULT;
        rc=legacy_ioctl_prepare(address,sizeof(uint64_t),false,-RELIEFOS_EFAULT);
        if(rc)return rc;
        if(!user_range_ok(address,sizeof(uint64_t)))return -RELIEFOS_EFAULT;
        receipt=slot ? kernel_malloc(sizeof(*receipt)) : &temporary;
        if(!receipt)return -RELIEFOS_ENOMEM;
        *receipt=(struct block_size_receipt){.address=address,.request=request,
            .disk=disk,.partition=partition};
        if(slot){slot->name=(char *)receipt;slot->flags=TASK_LOOKUP_BLOCK_SIZE_KIND;}
    }
    if(!receipt->complete) {
        uint64_t first_lba=0,sectors=0;
        rc=storage_disk_block_info(disk,partition,&first_lba,&sectors);
        if(rc<0)return rc;
        receipt->value=request==BLKGETSIZE64 ? sectors*512ULL : sectors;
        receipt->complete=true;
        storage_release_task_io(task->pid);
    }
    rc=legacy_ioctl_prepare(address,sizeof(uint64_t),true,-RELIEFOS_EFAULT);
    if(rc)return rc;
    if(!user_range_writable(address,sizeof(uint64_t)))return -RELIEFOS_EFAULT;
    *(uint64_t *)(uintptr_t)address=receipt->value;
    return 0;
}

struct fb_blit_stage {
    struct reliefos_fb_present update;
    uint32_t fb_width, fb_height, pitch, backend;
    uint8_t bpp, bytes_per_pixel, red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
    uint32_t row, column;
    uint32_t pixels[];
};

/** @brief Admit a readable framebuffer source span without faulting its gaps.
 * @param task Live execution-owned caller. @param address First user byte.
 * @param length Declared clipped span including stride gaps.
 * @return True for mapped/read-fault-eligible regions. VMA intervals are
 * skipped as a unit; unmapped gaps fail at their first page. The tmpfs size
 * query is its in-memory inode stat and no disk or swap I/O is issued here.
 */
static bool fb_access_span(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    if (!task || address < RELIEFNT_USER_BASE || address >= RELIEFNT_USER_TOP ||
        length > RELIEFNT_USER_TOP-address) return false;
    uint64_t end=address+length,page=address&~(PAGE_SIZE-1ULL);
    while(page<end) {
        struct task_vma *cover=NULL;uint64_t next=end;
        for(uint32_t i=0;i<sched_task_vma_capacity(task);++i) {
            struct task_vma *v=sched_task_vma_at(task,i);
            if(!v || !v->used)continue;
            if(page>=v->start && page<v->end)cover=v;
            else if(v->start>page && v->start<next)next=v->start;
        }
        bool faultable=cover && (writing ? (cover->prot&TASK_VMA_PROT_WRITE)!=0 : cover->prot!=0) &&
            ((cover->flags&(TASK_VMA_FLAG_ANON|TASK_VMA_FLAG_SYSV_SHM|TASK_VMA_FLAG_DEVICE|
                            TASK_VMA_FLAG_VDSO|TASK_VMA_FLAG_VVAR)) ||
             (cover->flags&(TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY))==
                (TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY));
        uint64_t until=cover ? cover->end : next;
        if(faultable && (cover->file_node.flags&STORAGE_NODE_FLAG_TMPFS) &&
           (cover->flags&(TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_MMAP))==
               (TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_MMAP)) {
            struct linux_stat_abi stat;
            if(storage_inode_stat(&cover->file_node,&stat)<0 || stat.st_size<0 ||
               cover->file_offset>UINT64_MAX-(page-cover->start))faultable=false;
            else {
                uint64_t offset=cover->file_offset+page-cover->start;
                uint64_t size=(uint64_t)stat.st_size;
                uint64_t rounded=size>UINT64_MAX-(PAGE_SIZE-1) ? UINT64_MAX :
                    (size+PAGE_SIZE-1)&~(PAGE_SIZE-1);
                if(offset>=rounded)faultable=false;
                else if(rounded-offset<until-page)until=page+(rounded-offset);
            }
        }
        if(faultable && until>page) { page=until;continue; }
        uint64_t flags=writing ? address_space_user_page_flags(sched_task_as(task),page) : 0;
        bool resident=writing ? address_space_user_page_writable(sched_task_as(task),page) ||
            ((flags&(RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_USER|RELIEFNT_PAGE_COW))==
             (RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_USER|RELIEFNT_PAGE_COW) &&
             !(flags&(RELIEFNT_PAGE_PROTNONE|RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_SHARED))) :
            address_space_user_page_readable(sched_task_as(task),page);
        if(resident) {
            page+=PAGE_SIZE;continue;
        }
        if(cover || paging_user_entry_kind(sched_task_as(task),page)==PAGING_USER_ENTRY_PROTNONE)
            return false;
        uint64_t stack_end=0;
        if(!mm_user_stack_readable_extent(task,page,&stack_end))return false;
        if(stack_end<next)next=stack_end;
        if(next<=page)return false;
        page=next;
    }
    return true;
}

/** @brief Check the initial framebuffer pixel span without faulting gaps.
 * @param task Current task. @param address First byte. @param length Byte span.
 * @return True if existing mappings/backing permit the read admission.
 */
static bool fb_readable_span(struct task *task,uint64_t address,uint64_t length)
{ return fb_access_span(task,address,length,false); }

struct fb_cmap_stage {
    struct fb_cmap request;
    uint32_t index, channel;
    uint16_t colors[4][256];
};

/** @brief Import a palette or return a saved palette in original store order.
 * @param task Current task. @param command GET or PUT cmap.
 * @param argument User header. @return Zero, ordinary errno or private wait.
 * Input snapshots and completed output values belong to the syscall scope;
 * no user address alias, VMA or physical page is retained across a wait.
 */
static int64_t fb_user_cmap(struct task *task,uint64_t command,uint64_t argument)
{
    struct task_user_text header;int rc=task_lookup_user_bytes(task,argument,sizeof(struct fb_cmap),&header);
    if(rc)return rc;
    struct fb_cmap cmap;__builtin_memcpy(&cmap,header.text,sizeof(cmap));kernel_free(header.allocation);
    if(cmap.start>256u || cmap.len>256u-cmap.start)return -RELIEFOS_EINVAL;
    if(!cmap.len)return 0;
    if(!cmap.red || !cmap.green || !cmap.blue)return -RELIEFOS_EFAULT;
    const uint64_t ptrs[]={cmap.red,cmap.green,cmap.blue,cmap.transp};
    if(command==FBIOPUTCMAP) {
        struct task_user_text colors[4]={{0}};int64_t result=0;
        for(unsigned i=0;i<4;++i)if(ptrs[i]) {
            result=task_lookup_user_bytes(task,ptrs[i],cmap.len*sizeof(uint16_t),&colors[i]);
            if(result)goto cmap_input_out;
        }
        for(uint32_t i=0;i<cmap.len;++i)for(unsigned c=0;c<4;++c) {
            uint16_t value=0;
            if(ptrs[c])__builtin_memcpy(&value,colors[c].text+i*sizeof(value),sizeof(value));
            reliefos_fb_palette[c][cmap.start+i]=value;
        }
cmap_input_out:
        for(unsigned i=0;i<4;++i)kernel_free(colors[i].allocation);
        return result;
    }
    struct task_lookup_path *slot=NULL;rc=task_lookup_scope_next(task,&slot);if(rc)return rc;
    struct fb_cmap_stage *stage;
    if(slot && slot->name) {
        if(slot->flags!=TASK_LOOKUP_FB_CMAP_KIND)return -RELIEFOS_EINVAL;
        stage=(struct fb_cmap_stage *)slot->name;
        if(__builtin_memcmp(&stage->request,&cmap,sizeof(cmap)))return -RELIEFOS_EINVAL;
    } else {
        /* Check all outputs before the first store without prefaulting them. */
        for(unsigned i=0;i<4;++i)if(ptrs[i] &&
            !fb_access_span(task,ptrs[i],cmap.len*sizeof(uint16_t),true))return -RELIEFOS_EFAULT;
        stage=kernel_malloc(sizeof(*stage));if(!stage)return -RELIEFOS_ENOMEM;
        *stage=(struct fb_cmap_stage){.request=cmap};
        for(unsigned c=0;c<4;++c)for(uint32_t i=0;i<cmap.len;++i)
            stage->colors[c][i]=reliefos_fb_palette[c][cmap.start+i];
        if(slot){slot->name=(char *)stage;slot->flags=TASK_LOOKUP_FB_CMAP_KIND;}
    }
    int64_t result=0;
    while(stage->index<cmap.len) {
        uint32_t c=stage->channel;
        if(ptrs[c]) {
            uint64_t address=ptrs[c]+stage->index*sizeof(uint16_t);
            rc=swap_prepare_user_range(task,address,sizeof(uint16_t),true);
            if(rc || !user_range_writable(address,sizeof(uint16_t))) {
                result=rc==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
                goto cmap_output_out;
            }
            __builtin_memcpy((void *)(uintptr_t)address,&stage->colors[c][stage->index],sizeof(uint16_t));
        }
        if(++stage->channel==4){stage->channel=0;++stage->index;}
    }
cmap_output_out:
    if(!slot)kernel_free(stage);
    return result;
}

/** @brief Retain a bounded clipped image and publish only after complete import.
 * @param task Current user task. @param fb Current framebuffer, never retained.
 * @param argument User request. @return Result, ordinary errno or private wait.
 * A retry owns copied metadata/pixels, not a VA alias or framebuffer pointer.
 */
static int64_t fb_user_blit(struct task *task,const struct framebuffer *fb,uint64_t argument)
{
    struct task_user_text text;
    int rc=task_lookup_user_bytes(task,argument,sizeof(struct reliefos_fb_present),&text);
    if(rc)return rc;
    struct reliefos_fb_present update;
    __builtin_memcpy(&update,text.text,sizeof(update));kernel_free(text.allocation);
    if(update.pixels && update.stride<update.width)return -RELIEFOS_EINVAL;
    if(!update.width || !update.height || update.x>=fb->width || update.y>=fb->height)return 0;
    if(update.width>fb->width-update.x)update.width=fb->width-update.x;
    if(update.height>fb->height-update.y)update.height=fb->height-update.y;
    if(!update.pixels) {
        framebuffer_rect(update.x,update.y,update.width,update.height,update.color);
        framebuffer_present_region(update.x,update.y,update.width,update.height);return 0;
    }
    uint64_t rows=update.height-1u;
    if(update.stride && rows>(UINT64_MAX-update.width)/update.stride)return -RELIEFOS_EFAULT;
    uint64_t span=rows*update.stride+update.width;
    if(span>UINT64_MAX/4u || update.pixels<RELIEFNT_USER_BASE ||
       update.pixels>=RELIEFNT_USER_TOP || span*4u>RELIEFNT_USER_TOP-update.pixels)
        return -RELIEFOS_EFAULT;
    uint64_t pixels=(uint64_t)update.width*update.height;
    if(pixels>(SIZE_MAX-sizeof(struct fb_blit_stage))/sizeof(uint32_t))return -RELIEFOS_ENOMEM;
    struct task_lookup_path *slot=NULL;rc=task_lookup_scope_next(task,&slot);if(rc)return rc;
    struct fb_blit_stage *stage;
    if(slot && slot->name) {
        if(slot->flags!=TASK_LOOKUP_FB_BLIT_KIND)return -RELIEFOS_EINVAL;
        stage=(struct fb_blit_stage *)slot->name;
        if(stage->fb_width!=fb->width || stage->fb_height!=fb->height || stage->pitch!=fb->pitch ||
           stage->backend!=fb->backend || stage->bpp!=fb->bpp || stage->bytes_per_pixel!=fb->bytes_per_pixel ||
           stage->red_pos!=fb->red_field_position || stage->red_size!=fb->red_mask_size ||
           stage->green_pos!=fb->green_field_position || stage->green_size!=fb->green_mask_size ||
           stage->blue_pos!=fb->blue_field_position || stage->blue_size!=fb->blue_mask_size ||
           __builtin_memcmp(&stage->update,&update,sizeof(update)))return -RELIEFOS_EAGAIN;
    } else {
        if(!fb_readable_span(task,update.pixels,span*4u))return -RELIEFOS_EFAULT;
        stage=kernel_malloc(sizeof(*stage)+(size_t)pixels*sizeof(uint32_t));
        if(!stage)return -RELIEFOS_ENOMEM;
        *stage=(struct fb_blit_stage){.update=update,.fb_width=fb->width,.fb_height=fb->height,
            .pitch=fb->pitch,.backend=fb->backend,.bpp=fb->bpp,.bytes_per_pixel=fb->bytes_per_pixel,
            .red_pos=fb->red_field_position,.red_size=fb->red_mask_size,
            .green_pos=fb->green_field_position,.green_size=fb->green_mask_size,
            .blue_pos=fb->blue_field_position,.blue_size=fb->blue_mask_size};
        if(slot){slot->name=(char *)stage;slot->flags=TASK_LOOKUP_FB_BLIT_KIND;}
    }
    int64_t result=0;
    while(stage->row<update.height) {
        uint32_t n=update.width-stage->column;if(n>32768u/4u)n=32768u/4u;
        uint64_t offset=((uint64_t)stage->row*update.stride+stage->column)*4u;
        uint64_t source=update.pixels+offset;uint64_t bytes=(uint64_t)n*4u;
        rc=swap_prepare_user_range(task,source,bytes,false);
        if(rc || !user_range_ok(source,bytes)) {
            result=rc==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto blit_out;
        }
        __builtin_memcpy(stage->pixels+(uint64_t)stage->row*update.width+stage->column,
                         (const void *)(uintptr_t)source,bytes);
        stage->column+=n;
        if(stage->column==update.width){stage->column=0;++stage->row;}
    }
    framebuffer_blit(update.x,update.y,update.width,update.height,update.width,stage->pixels);
    framebuffer_present_region(update.x,update.y,update.width,update.height);
blit_out:
    if(!slot)kernel_free(stage);
    return result;
}
#endif


/**
 * @brief Dispatch an already-normalised syscall within this ABI category.
 * @param number Linux or ReliefOS syscall number selected by the common dispatcher.
 * @param a0 First native ABI argument; pointers remain subject to the original checks.
 * @param a1 Second native ABI argument.
 * @param a2 Third native ABI argument.
 * @param a3 Fourth native ABI argument.
 * @param a4 Fifth native ABI argument.
 * @param a5 Sixth native ABI argument.
 * @return Original result, negative errno, or internal blocking/retry status.
 * @note The caller retains the kernel execution transaction and descriptor pins.
 */
int64_t syscall_legacy_ioctl(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_IOCTL) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        if (file && file->kind == TASK_FILE_KIND_INOTIFY) {
            if ((uint32_t)a1 == FIONREAD)
                return task_inotify_fionread ? task_inotify_fionread(task, file, a2) : -RELIEFOS_ENOSYS;
            return -LINUX_ENOTTY;
        }
        if (file && file->kind == TASK_FILE_KIND_SIGNALFD) {
            /* Generic ioctl requests have already been handled. */
            return -LINUX_ENOTTY;
        }
        /* Linux interface ioctls are independent of socket protocol. musl
         * uses an AF_UNIX datagram socket for if_nametoindex/if_indextoname. */
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX) &&
            a1 >= 0x8900 && a1 <= 0x89ff)
            return net_interface_ioctl((uint32_t)a1, a2);
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX))
            return task_socket_ioctl(file, a1, a2);
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_INET) && task_inet_family(file) == AF_INET6 &&
            (a1 == SIOCSIFADDR || a1 == SIOCDIFADDR || a1 == SIOCADDRT || a1 == SIOCDELRT))
            return net_ipv6_interface_ioctl((uint32_t)a1, a2);
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_INET))
            return task_net_control(file, (uint32_t)a1, a2);
        if (file && (file->node.flags & STORAGE_NODE_FLAG_DEV_NODE) &&
            file->node.first_cluster == STORAGE_DEV_KIND_DRIVERCTL) {
            return syscall_driver_control((uint32_t)a1, a2);
        }
        int evdev_ret = task_evdev_ioctl(file, a1, a2);
        if (evdev_ret != -RELIEFOS_ENOTTY) {
            return evdev_ret;
        }
    }

    if (number == LINUX_SYS_IOCTL) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        uint32_t disk_id;
        int32_t partition_index;
#ifndef CONFIG_SWAP
        uint64_t first_lba;
        uint64_t sector_count;
#endif
        if (task_block_device(file, &disk_id, &partition_index)) {
            if (a1 == BLKGETDISKSEQ) {
                IOCTL_PREPARE(a2, sizeof(uint64_t), true, -RELIEFOS_EFAULT);
                if (!a2 || !user_range_writable(a2, sizeof(uint64_t))) return -RELIEFOS_EFAULT;
                uint64_t sequence = block_device_sequence(&file->block_ref);
                if (!sequence) return -RELIEFOS_ENODEV;
                *(uint64_t *)(uintptr_t)a2 = sequence;
                return 0;
            }
            if (a1 == BLKGETSIZE64 || a1 == BLKGETSIZE) {
#ifdef CONFIG_SWAP
                return block_ioctl_size(task,disk_id,partition_index,a1,a2);
#else
                if (!a2 || !user_range_ok(a2, sizeof(uint64_t))) {
                    return -RELIEFOS_EFAULT;
                }
                {
                    int ret = storage_disk_block_info(disk_id, partition_index,
                                                      &first_lba, &sector_count);
                    if (ret < 0) return ret;
                }
                *(uint64_t *)(uintptr_t)a2 = a1 == BLKGETSIZE64
                                                  ? sector_count * 512ULL
                                                  : sector_count;
                return 0;
#endif
            }
            if (a1 == BLKSSZGET) {
                IOCTL_PREPARE(a2, sizeof(int), true, -RELIEFOS_EFAULT);
                if (!a2 || !user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
                struct block_device_info info;
                int result = block_device_info(&file->block_ref, &info);
                if (result) return result;
                *(int *)(uintptr_t)a2 = info.block_size;
                return 0;
            }
            if (a1 == BLKROGET) {
                IOCTL_PREPARE(a2, sizeof(int), true, -RELIEFOS_EFAULT);
                if (!a2 || !user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
                struct block_device_info info;
                int result = block_device_info(&file->block_ref, &info);
                if (result) return result;
                *(int *)(uintptr_t)a2 = !!(info.flags & BLOCK_DEVICE_READ_ONLY);
                return 0;
            }
            if (a1 == BLKRRPART) {
                /* Linux requires CAP_SYS_ADMIN for partition-table rescans. */
                if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) {
                    return -RELIEFOS_EACCES;
                }
                if (partition_index >= 0) return -RELIEFOS_EINVAL;
                return storage_disk_block_reread(disk_id);
            }
            if (a1 == BLKROSET) {
                return -RELIEFOS_EPERM;
            }
        }
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == FBIOGET_VSCREENINFO || a1 == FBIOPUT_VSCREENINFO ||
         a1 == FBIOGET_FSCREENINFO || a1 == FBIOPAN_DISPLAY ||
         a1 == FBIOGETCMAP || a1 == FBIOPUTCMAP || a1 == FBIOBLANK ||
         a1 == RELIEFOS_FBIOGET_CAPABILITIES || a1 == RELIEFOS_FBIOUPDATE_REGION ||
         a1 == RELIEFOS_FBIOBLIT)) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        const struct framebuffer *fb = framebuffer_get();
        if (!file || !(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
            file->node.first_cluster != STORAGE_DEV_KIND_FB0 || !fb ||
            !fb->available) {
            return -RELIEFOS_ENOTTY;
        }
        /* The native present path requires a live claim on the display: a
         * controlling VT, the active one, claimed in graphics mode.  The
         * standard fbdev mode set has no such gate in Linux — Xorg calls
         * FBIOPUT_VSCREENINFO during ScreenInit from a server process with
         * no controlling terminal — so it must keep working on any open
         * /dev/fb0 descriptor. */
        if (a1 == RELIEFOS_FBIOBLIT) {
            if (!task || !pty_vt_number(task->controlling_pty_id)) return -RELIEFOS_EPERM;
            if (pty_vt_active() != task->controlling_pty_id) return -RELIEFOS_EAGAIN;
            if (!pty_vt_graphical_active())
                return -RELIEFOS_EAGAIN;
        }
        if (a1 == RELIEFOS_FBIOBLIT) {
#ifdef CONFIG_SWAP
            return fb_user_blit(task,fb,a2);
#else
            struct reliefos_fb_present update;
            if (!user_range_ok(a2, sizeof(update))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(&update, (const void *)(uintptr_t)a2, sizeof(update));
            if (update.pixels && update.stride < update.width) return -RELIEFOS_EINVAL;
            if (!update.width || !update.height || update.x >= fb->width || update.y >= fb->height)
                return 0;
            if (update.width > fb->width - update.x) update.width = fb->width - update.x;
            if (update.height > fb->height - update.y) update.height = fb->height - update.y;
            if (update.pixels) {
                uint64_t bytes = ((uint64_t)(update.height - 1) * update.stride + update.width) * 4;
                if (!user_range_ok(update.pixels, bytes)) return -RELIEFOS_EFAULT;
                framebuffer_blit(update.x, update.y, update.width, update.height,
                                 update.stride, (const uint32_t *)(uintptr_t)update.pixels);
            } else {
                framebuffer_rect(update.x, update.y, update.width, update.height, update.color);
            }
            framebuffer_present_region(update.x, update.y, update.width, update.height);
            return 0;
#endif
        }
        if (a1 == FBIOBLANK) {
            if (a2 > FB_BLANK_POWERDOWN) return -RELIEFOS_EINVAL;
            reliefos_fb_blank = (uint32_t)a2;
            if (a2 == FB_BLANK_UNBLANK)
                framebuffer_present_region(0, 0, fb->width, fb->height);
            return 0;
        }
        if (a1 == FBIOGETCMAP || a1 == FBIOPUTCMAP) {
#ifdef CONFIG_SWAP
            return fb_user_cmap(task,a1,a2);
#else
            struct fb_cmap cmap;
            uint32_t end;
            IOCTL_PREPARE(a2, sizeof(cmap), false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, sizeof(cmap))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(&cmap, (const void *)(uintptr_t)a2, sizeof(cmap));
            if (cmap.start > 256u || cmap.len > 256u - cmap.start)
                return -RELIEFOS_EINVAL;
            end = cmap.start + cmap.len;
            if (!cmap.len) return 0;
            if (!cmap.red || !cmap.green || !cmap.blue)
                return -RELIEFOS_EFAULT;
            if (a1 == FBIOPUTCMAP) {
                if (!user_range_ok(cmap.red, cmap.len * sizeof(uint16_t)) ||
                    !user_range_ok(cmap.green, cmap.len * sizeof(uint16_t)) ||
                    !user_range_ok(cmap.blue, cmap.len * sizeof(uint16_t)) ||
                    (cmap.transp && !user_range_ok(cmap.transp,
                                                    cmap.len * sizeof(uint16_t))))
                    return -RELIEFOS_EFAULT;
                for (uint32_t i = cmap.start; i < end; ++i) {
                    reliefos_fb_palette[0][i] = ((const uint16_t *)(uintptr_t)cmap.red)[i - cmap.start];
                    reliefos_fb_palette[1][i] = ((const uint16_t *)(uintptr_t)cmap.green)[i - cmap.start];
                    reliefos_fb_palette[2][i] = ((const uint16_t *)(uintptr_t)cmap.blue)[i - cmap.start];
                    reliefos_fb_palette[3][i] = cmap.transp
                        ? ((const uint16_t *)(uintptr_t)cmap.transp)[i - cmap.start] : 0;
                }
            } else {
                if (!user_range_writable(cmap.red, cmap.len * sizeof(uint16_t)) ||
                    !user_range_writable(cmap.green, cmap.len * sizeof(uint16_t)) ||
                    !user_range_writable(cmap.blue, cmap.len * sizeof(uint16_t)) ||
                    (cmap.transp && !user_range_writable(cmap.transp,
                                                          cmap.len * sizeof(uint16_t))))
                    return -RELIEFOS_EFAULT;
                for (uint32_t i = cmap.start; i < end; ++i) {
                    ((uint16_t *)(uintptr_t)cmap.red)[i - cmap.start] = reliefos_fb_palette[0][i];
                    ((uint16_t *)(uintptr_t)cmap.green)[i - cmap.start] = reliefos_fb_palette[1][i];
                    ((uint16_t *)(uintptr_t)cmap.blue)[i - cmap.start] = reliefos_fb_palette[2][i];
                    if (cmap.transp)
                        ((uint16_t *)(uintptr_t)cmap.transp)[i - cmap.start] = reliefos_fb_palette[3][i];
                }
            }
            return 0;
#endif
        }
        /* Panning display is the Linux fbdev flush request: VMware SVGA
         * scanout only refreshes from VRAM when the FIFO receives an
         * update command, and mmap writers bypass every other path. */
        if (a1 == FBIOPAN_DISPLAY) {
            framebuffer_present_region(0, 0, fb->width, fb->height);
            return 0;
        }
        if (a1 == RELIEFOS_FBIOUPDATE_REGION) {
            uint32_t region[4];
            IOCTL_PREPARE(a2, sizeof(region), false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, sizeof(region))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(region, (const void *)(uintptr_t)a2, sizeof(region));
            framebuffer_present_region(region[0], region[1], region[2], region[3]);
            return 0;
        }
        if (!a2) {
            return -RELIEFOS_ENOTTY;
        }
        if (a1 == RELIEFOS_FBIOGET_CAPABILITIES) {
            IOCTL_PREPARE(a2, sizeof(struct reliefos_fb_capabilities), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(struct reliefos_fb_capabilities)))
                return -RELIEFOS_EFAULT;
            *(struct reliefos_fb_capabilities *)(uintptr_t)a2 =
                (struct reliefos_fb_capabilities){
                    .bytes_per_pixel = fb->bytes_per_pixel,
                    .capabilities = fb->capabilities,
                    .max_width = fb->max_width,
                    .max_height = fb->max_height,
                    .max_bytes = fb->max_bytes,
                    .backend = fb->backend,
                };
            return 0;
        }
        if (a1 == FBIOGET_VSCREENINFO) {
            struct fb_var_screeninfo info;
            IOCTL_PREPARE(a2, sizeof(info), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(info))) return -RELIEFOS_EFAULT;
            info = (struct fb_var_screeninfo){
                .xres = fb->width,
                .yres = fb->height,
                .xres_virtual = fb->width,
                .yres_virtual = fb->height,
                .bits_per_pixel = fb->bpp,
                .red = { .offset = fb->red_field_position,
                         .length = fb->red_mask_size },
                .green = { .offset = fb->green_field_position,
                           .length = fb->green_mask_size },
                .blue = { .offset = fb->blue_field_position,
                          .length = fb->blue_mask_size },
            };
            *(struct fb_var_screeninfo *)(uintptr_t)a2 = info;
            return 0;
        }
        if (a1 == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo info;
            IOCTL_PREPARE(a2, sizeof(info), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(info))) return -RELIEFOS_EFAULT;
            if (!framebuffer_fbdev_get_fix_info(fb, &info))
                return -RELIEFOS_EIO;
            *(struct fb_fix_screeninfo *)(uintptr_t)a2 = info;
            return 0;
        }
        return syscall_fbdev_put_var(a2);
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == VT_OPENQRY || a1 == VT_GETMODE || a1 == VT_SETMODE ||
         a1 == VT_GETSTATE || a1 == VT_RELDISP ||
         a1 == RELIEFOS_VT_GETGENERATION || a1 == VT_ACTIVATE ||
         a1 == VT_WAITACTIVE || a1 == KDGETMODE || a1 == KDSETMODE ||
         a1 == KDGKBMODE || a1 == KDSKBMODE)) {
        struct task *task = sched_current_task();
        struct task_pty_fd *endpoint = task_pty_for_io(task, (int)a0);
        struct task_file *file = task_file_for_io(task, (int)a0);
        uint32_t id = endpoint && endpoint->endpoint == TASK_PTY_ENDPOINT_SLAVE
                          ? endpoint->pty_id : task_device_is(file, STORAGE_DEV_KIND_CONSOLE)
                          ? (uint32_t)file->aux : 0u;
        if (!pty_vt_number(id)) return -RELIEFOS_ENOTTY;
        if (a1 == VT_OPENQRY) {
            return task_vt_query_ioctl(a1, a2);
        }
        if (a1 == RELIEFOS_VT_GETGENERATION) {
            IOCTL_PREPARE(a2, sizeof(uint64_t), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(uint64_t))) return -RELIEFOS_EFAULT;
            *(uint64_t *)(uintptr_t)a2 = pty_vt_generation();
            return 0;
        }
        if (a1 == VT_GETSTATE) {
            return task_vt_query_ioctl(a1, a2);
        }
        if (a1 == VT_GETMODE) {
            IOCTL_PREPARE(a2, sizeof(struct vt_mode), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(struct vt_mode))) return -RELIEFOS_EFAULT;
            return pty_vt_get_mode(id, (struct vt_mode *)(uintptr_t)a2);
        }
        if (a1 == VT_SETMODE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            IOCTL_PREPARE(a2, sizeof(struct vt_mode), false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, sizeof(struct vt_mode))) return -RELIEFOS_EFAULT;
            return pty_vt_set_mode(id, (const struct vt_mode *)(uintptr_t)a2);
        }
        if (a1 == VT_RELDISP) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            return pty_vt_release_display(id, (int)a2);
        }
        if (a1 == VT_ACTIVATE || a1 == VT_WAITACTIVE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            if (a2 < 1 || a2 > 6) return -RELIEFOS_EINVAL;
            return a1 == VT_ACTIVATE ? pty_vt_switch((uint32_t)a2)
                                    : pty_vt_wait_active((uint32_t)a2);
        }
        if (a1 == KDGETMODE) {
            IOCTL_PREPARE(a2, sizeof(int), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
            *(int *)(uintptr_t)a2 = pty_vt_graphical(id) ? KD_GRAPHICS : KD_TEXT;
            return 0;
        }
        if (a1 == KDGKBMODE) {
            IOCTL_PREPARE(a2, sizeof(int), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
            return pty_vt_get_keyboard_mode(id, (int *)(uintptr_t)a2);
        }
        if (a1 == KDSKBMODE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            return pty_vt_set_keyboard_mode(id, (int)a2);
        }
        /* KDSETMODE changes who owns the display, so it follows the same rule
         * as the other VT ioctls: the controlling terminal or the
         * CAP_SYS_TTY_CONFIG capability. */
        if (task->controlling_pty_id != id &&
            !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG)))
            return -RELIEFOS_EPERM;
        if (a2 != KD_TEXT && a2 != KD_GRAPHICS) return -RELIEFOS_EINVAL;
        return pty_vt_set_graphics(id, a2 == KD_GRAPHICS);
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == TCGETS || a1 == TCSETS || a1 == TCSETSW || a1 == TCSETSF ||
         a1 == LINUX_TCGETS2 || a1 == LINUX_TCSETS2 ||
         a1 == LINUX_TCSETSW2 || a1 == LINUX_TCSETSF2 ||
         a1 == TIOCGWINSZ || a1 == TIOCSWINSZ || a1 == TIOCGPTN ||
         a1 == TIOCSPTLCK || a1 == TIOCGPTLCK || a1 == TIOCSCTTY ||
         a1 == TIOCGPGRP || a1 == TIOCSPGRP || a1 == TIOCGSID || a1 == TIOCNOTTY)) {
        struct task *task = sched_current_task();
        struct task_pty_fd *endpoint = task_pty_for_io(task, (int)a0);
        struct task_file *file = task_file_for_io(task, (int)a0);
        int stream = task && task->syscall_file && task->syscall_fd == (int)a0
                         ? (task_device_is(task->syscall_file, STORAGE_DEV_KIND_TTY) ? 0 : -1)
                         : task_pty_stream_for_fd(task, (int)a0);
        uint32_t pty_id = endpoint ? endpoint->pty_id :
            task_device_is(file, STORAGE_DEV_KIND_CONSOLE) ? (uint32_t)file->aux :
            (stream >= 0 && task ? task->pty_id : 0);
        if (!pty_id || !pty_is_active(pty_id)) return -RELIEFOS_ENOTTY;
        int master = endpoint && endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER;
        if (!master && pty_is_hungup(pty_id))
            return a1 == TIOCSPGRP ? -RELIEFOS_ENOTTY : -RELIEFOS_EIO;
        if (a1 == TIOCNOTTY) {
            if (master) return -RELIEFOS_ENOTTY;
            return pty_detach_controlling(pty_id, task->pid);
        }
        if (a1 == TIOCSCTTY) {
            uint32_t access = endpoint ? task_pty_status(endpoint) & RELIEFOS_O_ACCMODE :
                file ? file->flags & RELIEFOS_O_ACCMODE : RELIEFOS_O_RDWR;
            return pty_acquire_controlling(pty_id, task->pid, (int32_t)a2,
                                           access != RELIEFOS_O_WRONLY);
        }
        if (a1 == TIOCGPGRP || a1 == TIOCGSID) {
            if (!master && task->controlling_pty_id != pty_id) return -RELIEFOS_ENOTTY;
            uint32_t value = 0;
            int result = a1 == TIOCGSID ? pty_get_session(pty_id, &value) :
                pty_get_foreground_pgid(pty_id, &value);
            if (result < 0) return result;
            IOCTL_PREPARE(a2, sizeof(value), true, -RELIEFOS_EFAULT);
            if (!user_range_writable(a2, sizeof(value))) return -RELIEFOS_EFAULT;
            *(uint32_t *)(uintptr_t)a2 = value;
            return 0;
        }
        if (a1 == TIOCSPGRP) {
            int64_t change = pty_check_change(pty_id, task->pid, 22);
            if (change) return change == -RELIEFOS_EIO ? -RELIEFOS_ENOTTY : change;
            IOCTL_PREPARE(a2, sizeof(uint32_t), false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, sizeof(uint32_t))) return -RELIEFOS_EFAULT;
            return pty_set_foreground_pgid(pty_id, task->pid, *(uint32_t *)(uintptr_t)a2);
        }
        if (a1 == TIOCGPTN) {
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER) return -RELIEFOS_EINVAL;
            IOCTL_PREPARE(a2, sizeof(uint32_t), true, -RELIEFOS_EINVAL);
            if (!user_range_writable(a2, sizeof(uint32_t))) return -RELIEFOS_EINVAL;
            *(uint32_t *)(uintptr_t)a2 = pty_id;
            return 0;
        }
        if (a1 == TIOCSPTLCK) {
            int locked;
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER) return -RELIEFOS_EINVAL;
            IOCTL_PREPARE(a2, sizeof(int), false, -RELIEFOS_EINVAL);
            if (!user_range_ok(a2, sizeof(int))) return -RELIEFOS_EINVAL;
            locked = *(int *)(uintptr_t)a2;
            if (locked != 0 && locked != 1) return -RELIEFOS_EINVAL;
            return pty_set_lock(pty_id, locked);
        }
        if (a1 == TIOCGPTLCK) {
            int locked;
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER) return -RELIEFOS_EINVAL;
            IOCTL_PREPARE(a2, sizeof(int), true, -RELIEFOS_EINVAL);
            if (!user_range_writable(a2, sizeof(int))) return -RELIEFOS_EINVAL;
            int ret = pty_get_lock(pty_id, &locked);
            if (ret < 0) return ret;
            *(int *)(uintptr_t)a2 = locked;
            return 0;
        }
        if (a1 == TIOCGWINSZ || a1 == TIOCSWINSZ) {
            struct linux_winsize native;
            if (a1 == TIOCGWINSZ) {
                IOCTL_PREPARE(a2, sizeof(native), true, -RELIEFOS_EFAULT);
                if (!user_range_writable(a2, sizeof(native))) return -RELIEFOS_EFAULT;
                int ret = pty_get_linux_winsize(pty_id, &native);
                if (ret < 0) return ret;
                *(struct linux_winsize *)(uintptr_t)a2 = native;
                return 0;
            }
            IOCTL_PREPARE(a2, sizeof(native), false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, sizeof(native))) return -RELIEFOS_EFAULT;
            native = *(const struct linux_winsize *)(uintptr_t)a2;
            return pty_set_linux_winsize(pty_id, &native);
        }
        {
            struct linux_termios2 termios;
            int extended = a1 == LINUX_TCGETS2 || a1 == LINUX_TCSETS2 ||
                           a1 == LINUX_TCSETSW2 || a1 == LINUX_TCSETSF2;
            int get = a1 == TCGETS || a1 == LINUX_TCGETS2;
            size_t length = extended ? sizeof(termios) : sizeof(struct linux_termios);
            int ret = pty_get_termios(pty_id, &termios);
            if (ret < 0) return ret;
            uint8_t *user = (uint8_t *)(uintptr_t)a2;
            uint8_t *native = (uint8_t *)&termios;
            if (get) {
                IOCTL_PREPARE(a2, length, true, -RELIEFOS_EFAULT);
                if (!user_range_writable(a2, length)) return -RELIEFOS_EFAULT;
                for (size_t i = 0; i < length; ++i) user[i] = native[i];
                return 0;
            }
            int64_t change = pty_check_change(pty_id, task->pid, 22);
            if (change) return change;
            IOCTL_PREPARE(a2, length, false, -RELIEFOS_EFAULT);
            if (!user_range_ok(a2, length)) return -RELIEFOS_EFAULT;
            for (size_t i = 0; i < length; ++i) native[i] = user[i];
            if (a1 == TCSETSF || a1 == LINUX_TCSETSF2) pty_flush_input(pty_id);
            return pty_set_termios(pty_id, &termios);
        }
    }

    if (number == LINUX_SYS_IOCTL) {
        /* Linux reports ENOTTY when neither do_vfs_ioctl() nor the descriptor's
         * ->unlocked_ioctl/->compat_ioctl handler understands a request, and
         * every device backend above uses the same code.  ENOSYS would claim
         * the ioctl syscall itself does not exist. */
        return -RELIEFOS_ENOTTY;
    }

    return -RELIEFOS_ENOSYS;
}

#undef IOCTL_PREPARE
