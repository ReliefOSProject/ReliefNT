/*
 * ReliefOS ELF loader: validates and maps static and dynamic x86_64 images.
 * Enforces ABI, W^X, PT_INTERP, segment-layout, and ASLR startup rules.
 */
#include <reliefos/elf_abi.h>
#include <reliefnt/console.h>
#include <reliefnt/elf.h>
#include <reliefnt/exec_file.h>
#include <reliefnt/syscall_internal.h>
#include <linux/fcntl.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/permissions.h>
#include <reliefnt/random.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PAGE_SIZE 4096ULL
#define ELF_HEADER_READ_BYTES 4096U
#define ELF_DYN_MAIN_MIN 0x01000000ULL
#define ELF_DYN_MAIN_MAX 0x04000000ULL
#define ELF_DYN_INTERP_MIN 0x04000000ULL
#define ELF_DYN_INTERP_MAX 0x06000000ULL

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf64_nhdr {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
};

static uint8_t elf_header_scratch[ELF_HEADER_READ_BYTES];

/**
 * @brief Round value down to the previous 4096-byte page boundary.
 */
static uint64_t align_down(uint64_t value)
{
    return value & ~(PAGE_SIZE - 1ULL);
}

/**
 * @brief Round value up to the next page boundary; returns 0 on overflow.
 */
static uint64_t align_up(uint64_t value)
{
    if (value > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return 0;
    }
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

/**
 * @brief Round value up to a 4-byte boundary; returns 0 on overflow.
 */
static uint64_t align4_up(uint64_t value)
{
    if (value > UINT64_MAX - 3ULL) {
        return 0;
    }
    return (value + 3ULL) & ~3ULL;
}

/**
 * @brief Return true if every program header fits within len bytes of the image.
 */
static bool elf64_program_headers_fit(const struct elf64_ehdr *eh, size_t len)
{
    if (!eh || eh->e_phentsize != sizeof(struct elf64_phdr) || eh->e_phoff > len) {
        return false;
    }
    return eh->e_phnum <= (len - eh->e_phoff) / eh->e_phentsize;
}

/**
 * @brief Return a pointer to the index-th program header within image.
 */
static const struct elf64_phdr *elf64_phdr_at(const struct elf64_ehdr *eh,
                                               const void *image, uint16_t index)
{
    return (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff +
                                       (uint64_t)index * eh->e_phentsize);
}

/**
 * @brief Scan PT_NOTE segments for the ReliefOS ABI note; store its major version and return true only when it matches the kernel's ABI major.
 */
static bool elf64_note_abi(const struct elf64_ehdr *eh, const void *image, size_t len,
                           uint32_t *out_major)
{
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        uint64_t cursor;
        uint64_t end;
        if (ph->p_type != PT_NOTE || ph->p_offset > len || ph->p_filesz > len - ph->p_offset) {
            continue;
        }
        cursor = ph->p_offset;
        end = ph->p_offset + ph->p_filesz;
        while (cursor + sizeof(struct elf64_nhdr) <= end) {
            const struct elf64_nhdr *note =
                (const struct elf64_nhdr *)((const uint8_t *)image + cursor);
            uint64_t names = cursor + sizeof(*note);
            uint64_t desc = align4_up(names + note->n_namesz);
            uint64_t next = align4_up(desc + note->n_descsz);
            if (!desc || !next || desc > end || next > end) {
                return false;
            }
            if (note->n_type == RELIEFOS_ELF_NOTE_TYPE &&
                note->n_namesz == sizeof(RELIEFOS_ELF_NOTE_NAME) &&
                note->n_descsz >= sizeof(struct reliefos_elf_abi_note)) {
                const char *name = (const char *)image + names;
                const struct reliefos_elf_abi_note *abi =
                    (const struct reliefos_elf_abi_note *)((const uint8_t *)image + desc);
                bool match = true;
                for (uint32_t n = 0; n < sizeof(RELIEFOS_ELF_NOTE_NAME); ++n) {
                    if (name[n] != RELIEFOS_ELF_NOTE_NAME[n]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    *out_major = abi->major;
                    return abi->major == RELIEFOS_ELF_ABI_MAJOR;
                }
            }
            cursor = next;
        }
    }
    return false;
}

/**
 * @brief Copy the PT_INTERP path string (if any) into out; returns false when absent or not NUL-terminated within the segment.
 */
static bool elf64_interp(const struct elf64_ehdr *eh, const void *image, size_t len,
                         char out[RELIEFOS_FS_PATH_LEN])
{
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        if (ph->p_type != PT_INTERP || !ph->p_filesz || ph->p_filesz > RELIEFOS_FS_PATH_LEN ||
            ph->p_offset > len || ph->p_filesz > len - ph->p_offset) {
            continue;
        }
        const char *src = (const char *)image + ph->p_offset;
        uint64_t j = 0;
        while (j < ph->p_filesz && src[j]) {
            out[j] = src[j];
            ++j;
        }
        if (j == ph->p_filesz) {
            return false;
        }
        out[j] = 0;
        return true;
    }
    return false;
}

/**
 * @brief Read interpreter and ABI metadata independently of ET_EXEC/ET_DYN.
 */
static bool elf64_validate_dynamic_header(const struct elf64_ehdr *eh, const void *image,
                                          size_t len, bool is_interpreter,
                                          struct elf_image_info *out)
{
    bool dynamic = false;
    bool phdr = out->phdr_vaddr != 0;
    bool interp = false;
    uint32_t abi_major = 0;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        if (ph->p_type == PT_DYNAMIC) {
            dynamic = true;
        } else if (ph->p_type == PT_PHDR) {
            phdr = true;
            out->phdr_vaddr = ph->p_vaddr;
        } else if (ph->p_type == PT_INTERP) {
            interp = true;
        }
    }
    bool legacy = elf64_note_abi(eh, image, len, &abi_major);
    if ((!is_interpreter && !dynamic) || !phdr) {
        return false;
    }
    if (!is_interpreter) {
        if (interp) {
            if (!elf64_interp(eh, image, len, out->interp) || !out->interp[0]) {
                return false;
            }
            if (legacy) {
                const char *expected = RELIEFOS_ELF_INTERP_PATH;
                for (uint32_t i = 0; expected[i] || out->interp[i]; ++i) {
                    if (expected[i] != out->interp[i]) return false;
                }
            }
        }
        /* An ET_DYN image without PT_INTERP is a static PIE: Linux maps it
         * directly with a load bias and starts at e_entry.  Keep accepting
         * it here; a truly dynamic image that lacks its interpreter faults
         * in userspace exactly as it would on Linux. */
    } else if (interp) {
        return false;
    }
    out->abi_major = legacy ? abi_major : 0;
    return true;
}

/**
 * @brief header_len is the amount available in image. file_len is the full backing file size, which is larger when the kernel lazily maps an ELF from storage.
 */
static bool elf64_probe_image(const void *image, size_t header_len, uint64_t file_len,
                              bool is_interpreter, struct elf_image_info *out)
{
    const struct elf64_ehdr *eh;
    uint32_t loads = 0;
    bool has_interpreter = false;
    if (!image || header_len < sizeof(struct elf64_ehdr) || !out) {
        return false;
    }
    *out = (struct elf_image_info){0};
    eh = (const struct elf64_ehdr *)image;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 || eh->e_ident[5] != 1 ||
        eh->e_ident[6] != 1 || (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_machine != EM_X86_64 || eh->e_version != 1 ||
        eh->e_ehsize != sizeof(*eh) || !elf64_program_headers_fit(eh, header_len)) {
        return false;
    }
    out->dynamic = eh->e_type == ET_DYN;
    if (is_interpreter && eh->e_type != ET_DYN) {
        return false;
    }
    out->entry = eh->e_entry;
    out->machine = eh->e_machine;
    out->phnum = eh->e_phnum;
    /* PT_PHDR is optional. Infer AT_PHDR from the LOAD containing the table. */
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        uint64_t bytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
        if (ph->p_type == PT_LOAD && eh->e_phoff >= ph->p_offset &&
            eh->e_phoff - ph->p_offset <= ph->p_filesz &&
            bytes <= ph->p_filesz - (eh->e_phoff - ph->p_offset)) {
            out->phdr_vaddr = ph->p_vaddr + eh->e_phoff - ph->p_offset;
            break;
        }
    }
    out->low_vaddr = UINT64_MAX;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        uint64_t end;
        if (ph->p_type == PT_INTERP) has_interpreter = true;
        if (ph->p_type != PT_LOAD || !ph->p_memsz) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz || ph->p_memsz > UINT64_MAX - ph->p_vaddr ||
            ph->p_offset > file_len || ph->p_filesz > file_len - ph->p_offset ||
            (ph->p_flags & PF_W && ph->p_flags & PF_X)) {
            return false;
        }
        end = ph->p_vaddr + ph->p_memsz;
        if (ph->p_vaddr < out->low_vaddr) {
            out->low_vaddr = ph->p_vaddr;
        }
        if (end > out->high_vaddr) {
            out->high_vaddr = end;
        }
        ++loads;
    }
    if (!loads || out->low_vaddr == UINT64_MAX) {
        return false;
    }
    if ((out->dynamic || has_interpreter) &&
        !elf64_validate_dynamic_header(eh, image, header_len, is_interpreter, out)) {
        return false;
    }
    out->valid = true;
    return true;
}

/**
 * @brief Validate image as an ELF64 x86_64 executable and fill out; wrapper that treats the whole buffer as the file.
 */
bool elf64_probe(const void *image, size_t len, struct elf_image_info *out)
{
    return elf64_probe_image(image, len, len, false, out);
}

/**
 * @brief Return true if the LOAD segment fits the file and, at bias, stays inside user address space without overflow.
 */
static bool elf64_segment_valid(const struct elf64_phdr *ph, uint64_t file_len,
                                uint64_t bias)
{
    uint64_t start;
    if (!ph || ph->p_filesz > ph->p_memsz || ph->p_offset > file_len ||
        ph->p_filesz > file_len - ph->p_offset || ph->p_memsz > UINT64_MAX - ph->p_vaddr ||
        ph->p_vaddr > UINT64_MAX - bias) {
        return false;
    }
    start = bias + ph->p_vaddr;
    return start >= RELIEFNT_USER_BASE && ph->p_memsz <= RELIEFNT_USER_TOP - start;
}

/**
 * @brief Allocate and map physical pages to back [start, end) in the address space, leaving already-mapped pages alone; returns false on failure.
 */
static bool ensure_segment_pages(struct address_space *as, uint64_t start,
                                 uint64_t end, uint64_t flags)
{
    for (uint64_t page = align_down(start); page < align_up(end); page += PAGE_SIZE) {
        uint64_t phys;
        if (address_space_user_page_phys(as, page)) {
            continue;
        }
        phys = mm_alloc_page();
        if (!phys || !address_space_map_user_page(as, page, phys, flags)) {
            if (phys) {
                mm_free_page(phys);
            }
            return false;
        }
    }
    return true;
}

/**
 * @brief Copy executable image bytes through physical aliases of a prepared user range.
 * @param as Owned address space whose destination pages are already mapped.
 * @param vaddr First destination user address.
 * @param src Readable kernel source containing len bytes.
 * @param len Number of bytes to copy across page boundaries.
 * @return True on success, false if any destination page has no CPU alias;
 * previously copied bytes remain written. Requires execution ownership.
 */
static bool copy_to_address_space(struct address_space *as, uint64_t vaddr,
                                  const uint8_t *src, uint64_t len)
{
    uint64_t copied = 0;
    while (copied < len) {
        uint64_t address = vaddr + copied;
        uint64_t phys = address_space_user_page_phys(as, address);
        uint64_t page_left = PAGE_SIZE - (address & (PAGE_SIZE - 1ULL));
        uint64_t take = len - copied < page_left ? len - copied : page_left;
        uint8_t *dst;
        if (!phys) {
            return false;
        }
        dst = (uint8_t *)mm_phys_to_virt(phys);
        if (!dst) return false;
        dst += address & (PAGE_SIZE - 1ULL);
        for (uint64_t i = 0; i < take; ++i) {
            dst[i] = src[copied + i];
        }
        copied += take;
    }
    return true;
}

/**
 * @brief Map a static ELF image into the address space: probe it, then allocate pages and copy each LOAD segment; rejects dynamic images.
 */
bool elf64_load_address_space(struct address_space *as, const void *image, size_t len,
                              struct elf_image_info *out)
{
    const struct elf64_ehdr *eh;
    if (!as || !elf64_probe(image, len, out) || out->dynamic) {
        return false;
    }
    eh = (const struct elf64_ehdr *)image;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        uint64_t flags = (ph->p_flags & PF_W) ? RELIEFNT_PAGE_WRITABLE : 0;
        if (ph->p_type != PT_LOAD || !ph->p_memsz) {
            continue;
        }
        if (!elf64_segment_valid(ph, len, 0)) {
            out->valid = false;
            return false;
        }
        if (!(ph->p_flags & PF_X)) {
            flags |= RELIEFNT_PAGE_NOEXEC;
        }
        if (!ensure_segment_pages(as, ph->p_vaddr, ph->p_vaddr + ph->p_memsz, flags) ||
            !copy_to_address_space(as, ph->p_vaddr,
                                   (const uint8_t *)image + ph->p_offset, ph->p_filesz)) {
            out->valid = false;
            return false;
        }
    }
    return true;
}

/**
 * @brief Read headers through the retained immutable ELF provider.
 * @param source Borrowed executable source holding its provider and writer denial.
 * @param out_image Receives scratch bytes valid until the next loader read.
 * @param out_len Receives the completed header byte count.
 * @return Zero or ENOEXEC/provider errno. Caller owns the loader transaction;
 * no graph/module lock spans I/O and no displayed pathname is reopened.
 */
static int elf64_read_headers(const struct vfs_exec_source *source, const void **out_image,
                               size_t *out_len)
{
    const struct storage_node *node=vfs_exec_source_node(source);
    const struct elf64_ehdr *eh;
    uint64_t header_len;
    uint32_t wanted;
    uint32_t got = 0;
    if (!node || node->type != RELIEFOS_FS_TYPE_FILE || node->size < sizeof(struct elf64_ehdr) ||
        !out_image || !out_len) {
        return -8;
    }
    int ret = vfs_exec_source_read(source, 0, elf_header_scratch, sizeof(struct elf64_ehdr), &got);
    if (ret < 0) return ret;
    if (got != sizeof(struct elf64_ehdr)) return -8;
    eh = (const struct elf64_ehdr *)elf_header_scratch;
    if (eh->e_phentsize != sizeof(struct elf64_phdr) || eh->e_phoff > node->size ||
        eh->e_phnum > (node->size - eh->e_phoff) / eh->e_phentsize) {
        return -8;
    }
    header_len = eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    wanted = (uint32_t)(node->size < ELF_HEADER_READ_BYTES ? node->size : ELF_HEADER_READ_BYTES);
    if (header_len > wanted || header_len < sizeof(*eh)) {
        return -8;
    }
    got = 0;
    ret = vfs_exec_source_read(source, 0, elf_header_scratch, wanted, &got);
    if (ret < 0) return ret;
    if (got != wanted) return -8;
    *out_image = elf_header_scratch;
    *out_len = wanted;
    return 0;
}

/** @brief Validate canonical user bounds and reject the supervisor kernel-image window.
 * @param start Inclusive first byte of the page-aligned segment range.
 * @param end Exclusive end of the segment range.
 * @return True only for a nonempty range wholly inside the low canonical user
 * half and outside the supervisor-only kernel hole.
 */
static bool elf64_user_range_valid(uint64_t start, uint64_t end)
{
    return start >= RELIEFNT_USER_BASE && start < end && end <= RELIEFNT_USER_TOP &&
           !(start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START);
}

/**
 * @brief Check an ELF reservation against user bounds, the stack and existing VMAs.
 * @param task Task whose private address space is stable under the execution lock.
 * @param start Inclusive virtual address of the proposed reservation.
 * @param end Exclusive virtual address of the proposed reservation.
 * @return True for a valid, nonoverlapping interval; false with a diagnostic
 * for an invalid task, range or collision. No page tables are allocated.
 */
static bool elf64_task_range_available(const struct task *task, uint64_t start, uint64_t end)
{
    uint64_t stack_low;
    if (!task || !elf64_user_range_valid(start, end) ||
        task->stack_top < (uint64_t)RELIEFNT_USER_STACK_PAGES * PAGE_SIZE) {
        console_error("[reliefnt] ELF range invalid start=0x%llx end=0x%llx stack=0x%llx\n",
                       (unsigned long long)start, (unsigned long long)end,
                       task ? (unsigned long long)task->stack_top : 0ULL);
        return false;
    }
    stack_low = task->stack_top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE;
    if (end > stack_low) {
        console_error("[reliefnt] ELF range overlaps stack start=0x%llx end=0x%llx stack=0x%llx\n",
                       (unsigned long long)start, (unsigned long long)end,
                       (unsigned long long)stack_low);
        return false;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
        if (vma && vma->used && start < vma->end && end > vma->start) {
            console_error("[reliefnt] ELF range overlaps VMA=%u range=0x%llx-0x%llx existing=0x%llx-0x%llx\n",
                           i, (unsigned long long)start, (unsigned long long)end,
                           (unsigned long long)vma->start, (unsigned long long)vma->end);
            return false;
        }
    }
    return true;
}

/**
 * @brief Return the first unused VMA slot of the task, or NULL when none remain.
 */
static struct task_vma *elf64_task_free_vma(struct task *task)
{
    for (uint32_t i = 0; task && i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && !vma->used) {
            return vma;
        }
    }
    return task ? sched_task_vma_at(task, sched_task_vma_capacity(task)) : NULL;
}

/**
 * @brief Validate each LOAD segment's range and file/va page alignment; when strict, also require their page ranges to be pairwise disjoint.
 */
static bool elf64_segments_nonoverlapping(const struct elf64_ehdr *eh, const void *image,
                                          uint64_t file_len, uint64_t bias,
                                          bool strict_page_layout)
{
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *left = elf64_phdr_at(eh, image, i);
        uint64_t left_start;
        uint64_t left_end;
        if (left->p_type != PT_LOAD || !left->p_memsz) {
            continue;
        }
        if (!elf64_segment_valid(left, file_len, bias) ||
            left->p_offset % PAGE_SIZE != left->p_vaddr % PAGE_SIZE) {
            return false;
        }
        left_start = align_down(bias + left->p_vaddr);
        left_end = align_up(bias + left->p_vaddr + left->p_memsz);
        if (!left_end) {
            return false;
        }
        if (!strict_page_layout) {
            continue;
        }
        for (uint16_t j = 0; j < i; ++j) {
            const struct elf64_phdr *right = elf64_phdr_at(eh, image, j);
            uint64_t right_start;
            uint64_t right_end;
            if (right->p_type != PT_LOAD || !right->p_memsz) {
                continue;
            }
            right_start = align_down(bias + right->p_vaddr);
            right_end = align_up(bias + right->p_vaddr + right->p_memsz);
            if (left_start < right_end && left_end > right_start) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Older ET_EXEC images can place a tiny writable GOT at the tail of a read-only file page. They remain supported by recording one VMA for that common file page, with the union of their page permissions. PIE images must keep page-granular LOAD ranges disjoint so their protections are exact from the first fault onward.
 */
static struct task_vma *elf64_legacy_shared_page_vma(struct task *task,
                                                      const struct storage_node *node,
                                                      uint64_t start, uint64_t end,
                                                      uint64_t file_offset)
{
    for (uint32_t i = 0; task && i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma) continue;
        uint64_t expected_offset;
        if (!vma->used || end <= vma->start || start >= vma->end) {
            continue;
        }
        if (!(vma->flags & TASK_VMA_FLAG_FILE) || start < vma->start ||
            vma->file_node.type != node->type || vma->file_node.volume_id != node->volume_id ||
            vma->file_node.first_cluster != node->first_cluster ||
            start - vma->start > UINT64_MAX - vma->file_offset) {
            return NULL;
        }
        expected_offset = vma->file_offset + start - vma->start;
        return expected_offset == file_offset ? vma : NULL;
    }
    return NULL;
}

/**
 * @brief Pick a random 2 MiB-aligned load bias inside the main/interpreter ASLR window that fits the image; returns 0 when the layout cannot fit.
 */
static uint64_t elf64_choose_bias(const struct elf_image_info *info, bool interpreter)
{
    uint64_t min = interpreter ? ELF_DYN_INTERP_MIN : ELF_DYN_MAIN_MIN;
    uint64_t max = interpreter ? ELF_DYN_INTERP_MAX : ELF_DYN_MAIN_MAX;
    uint64_t span;
    uint64_t base;
    uint64_t random;
    if (!info || info->high_vaddr <= info->low_vaddr ||
        info->high_vaddr - info->low_vaddr > max - min) {
        return 0;
    }
    span = (max - min - (info->high_vaddr - info->low_vaddr)) & ~0x1fffffULL;
    if (kernel_random_fill(&random, sizeof(random)) < 0) return 0;
    base = min + (random % (span / 0x200000ULL + 1ULL)) * 0x200000ULL;
    if (base < align_down(info->low_vaddr)) {
        return 0;
    }
    return base - align_down(info->low_vaddr);
}

/**
 * @brief Create lazy file-backed VMAs for every LOAD segment of image at bias, coalescing shared read-only pages of static ET_EXEC images.
 * @param task Pinned task whose address space and VMA metadata are owned by the caller.
 * @param source Borrowed immutable backing retaining its actual provider and writer denial.
 * @param image Validated ELF header and program-header bytes.
 * @param info Image metadata receiving the highest writable segment end.
 * @param bias Validated relocation bias for the image.
 * @param image_name Resident diagnostic name for failure messages.
 * @return Zero after VMA publication or a negative executable/allocation error.
 * Page tables and backing pages are populated by faults, not by the virtual span.
 */
static int elf64_map_one(struct task *task, struct vfs_exec_source *source,
                          const void *image, struct elf_image_info *info,
                          uint64_t bias, const char *image_name)
{
    const struct storage_node *node=vfs_exec_source_node(source);
    const struct elf64_ehdr *eh = image;
    uint32_t loads = 0;
    uint32_t free_vmas = 0;
    uint64_t program_break = 0;
    if (!elf64_segments_nonoverlapping(eh, image, node->size, bias, info->dynamic)) {
        console_error("[reliefnt] ELF %s segment layout rejected bias=0x%llx\n",
                       image_name, (unsigned long long)bias);
        return -8;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && !vma->used) {
            ++free_vmas;
        }
    }
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        if (ph->p_type == PT_LOAD && ph->p_memsz) {
            ++loads;
        }
    }
    if (!loads || loads > free_vmas) {
        console_error("[reliefnt] ELF %s VMA capacity rejected loads=%u free=%u\n",
                       image_name, loads, free_vmas);
        return -12;
    }
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = elf64_phdr_at(eh, image, i);
        struct task_vma *vma;
        struct storage_node segment_node;
        uint64_t start;
        uint64_t end;
        uint64_t file_offset;
        uint64_t page_delta;
        struct task_vma *legacy_vma = NULL;
        uint32_t prot = TASK_VMA_PROT_READ;
        if (ph->p_type != PT_LOAD || !ph->p_memsz) {
            continue;
        }
        start = align_down(bias + ph->p_vaddr);
        end = align_up(bias + ph->p_vaddr + ph->p_memsz);
        if (!end) {
            console_error("[reliefnt] ELF %s LOAD[%u] address overflow\n", image_name, i);
            return -8;
        }
        page_delta = ph->p_vaddr - align_down(ph->p_vaddr);
        if (ph->p_offset < page_delta) {
            console_error("[reliefnt] ELF %s LOAD[%u] file offset rejected\n", image_name, i);
            return -8;
        }
        file_offset = ph->p_offset - page_delta;
        if (!info->dynamic) {
            legacy_vma = elf64_legacy_shared_page_vma(task, node, start, end, file_offset);
        }
        if (!elf64_user_range_valid(start, end)) {
            console_error("[reliefnt] ELF %s LOAD[%u] user range rejected 0x%llx-0x%llx\n",
                           image_name, i, (unsigned long long)start,
                           (unsigned long long)end);
            return -8;
        }
        if (!legacy_vma && !elf64_task_range_available(task, start, end)) {
            console_error("[reliefnt] ELF %s LOAD[%u] range rejected 0x%llx-0x%llx\n",
                           image_name, i, (unsigned long long)start,
                           (unsigned long long)end);
            return -8;
        }
        vma = legacy_vma ? legacy_vma : elf64_task_free_vma(task);
        if (!vma) {
            console_error("[reliefnt] ELF %s LOAD[%u] VMA capacity rejected\n", image_name, i);
            return -12;
        }
        if (ph->p_flags & PF_W) {
            prot |= TASK_VMA_PROT_WRITE;
        }
        if (ph->p_flags & PF_X) {
            prot |= TASK_VMA_PROT_EXEC;
        }
        if ((ph->p_flags & PF_W) && end > program_break) {
            program_break = end;
        }
        if (legacy_vma) {
            if ((legacy_vma->prot | prot) & TASK_VMA_PROT_WRITE &&
                (legacy_vma->prot | prot) & TASK_VMA_PROT_EXEC) {
                console_error("[reliefnt] ELF %s LOAD[%u] legacy W+X page rejected\n",
                               image_name, i);
                return -8;
            }
            legacy_vma->prot |= prot;
            legacy_vma->max_prot |= prot;
            legacy_vma->flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
                                TASK_VMA_FLAG_PRIVATE;
            if (end > legacy_vma->end) {
                legacy_vma->end = end;
            }
            if (ph->p_offset + ph->p_filesz > legacy_vma->file_limit) {
                legacy_vma->file_limit = ph->p_offset + ph->p_filesz;
            }
            continue;
        }
        segment_node = *node;
        *vma = (struct task_vma){
            .used = 1,
            .prot = prot,
            .max_prot = prot,
            /* Read-only ELF pages are immutable and can be backed directly
             * by the system page cache.  Writable segments remain private so
             * fork/COW preserves process isolation. */
            .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
                     ((ph->p_flags & PF_W) ? TASK_VMA_FLAG_PRIVATE :
                      TASK_VMA_FLAG_SHARED_FILE),
            .start = start,
            .end = end,
            .file_offset = file_offset,
            .file_limit = ph->p_offset + ph->p_filesz,
            .file_node = segment_node,
            .exec_source = vfs_exec_source_get(source),
        };
    }
    info->program_break = program_break;
    return 0;
}

/**
 * @brief Map ELF LOADs and resolve PT_INTERP through actual task paths.
 * @param task Staged or unpublished task with owned MM/root/cwd state.
 * @param image_path Borrowed main executable retaining its actual provider.
 * @param out Receives metadata only after every image is mapped successfully.
 * @return Zero or negative executable/provider/permission errno. Failure leaves
 * partial VMAs for the caller's staged-MM rollback; each owns its immutable
 * backing source. The loader/execution transaction is required. Current user
 * syscall operands retain interpreter lookup/guards across EAGAIN; no graph or
 * module lock spans I/O. No diagnostic path or legacy text router is consulted.
 */
int elf64_map_task_image(struct task *task, const struct vfs_path *image_path,
                          struct elf_image_info *out)
{
    const void *image,*interp_image;
    size_t image_len,interp_len;
    struct elf_image_info main_info,interp_info;
    struct vfs_exec_source *main_source=NULL,*interp_source=NULL;
    struct vfs_lookup_result interpreter={0};
    struct storage_inode_ref *temporary=NULL;
    uint64_t main_bias=0,interp_bias;
    bool entry_found=false;
    int ret;
    if(!task || !image_path || !out)return -22;
    ret=vfs_exec_source_open(image_path,&main_source);
    if(ret)return ret;
    const struct storage_node *node=vfs_exec_source_node(main_source);
    ret=elf64_read_headers(main_source,&image,&image_len);
    if(ret)goto done;
    if(!elf64_probe_image(image,image_len,node->size,false,&main_info)){ret=-8;goto done;}
    if(main_info.dynamic) {
        main_bias=elf64_choose_bias(&main_info,false);
        if(!main_bias || main_info.entry>UINT64_MAX-main_bias ||
           main_info.phdr_vaddr>UINT64_MAX-main_bias){ret=-8;goto done;}
    }
    const struct elf64_ehdr *main_eh=image;
    for(uint16_t i=0;i<main_eh->e_phnum;++i) {
        const struct elf64_phdr *ph=elf64_phdr_at(main_eh,image,i);
        if(ph->p_type==PT_LOAD && (ph->p_flags&PF_X) && main_info.entry>=ph->p_vaddr &&
           main_info.entry-ph->p_vaddr<ph->p_memsz)entry_found=true;
    }
    if(!entry_found){ret=-8;goto done;}
    ret=elf64_map_one(task,main_source,image,&main_info,main_bias,"main");
    if(ret)goto done;
    main_info.load_bias=main_bias;main_info.entry+=main_bias;main_info.phdr_vaddr+=main_bias;
    if(!main_info.interp[0]) {
        main_info.interpreter_entry=main_info.entry;*out=main_info;goto done;
    }
    /* A staged exec task has no syscall scope. The actual caller retains the
     * same pre-commit root/cwd plus entered components on I/O retries. */
    struct task *caller=sched_current_task();
    if(!caller || caller->kind!=TASK_KIND_USER)caller=task;
    ret=resolve_kernel_object_at_flags(vfs_path_namespace(image_path),caller,LINUX_AT_FDCWD,
        main_info.interp,false,false,VFS_LOOKUP_FOLLOW,&interpreter);
    if(ret)goto done;
    ret=fs_permissions_check_path(task,&interpreter.path,FS_ACCESS_EXEC,false);
    if(ret)goto done;
    if(fs_permissions_check_path(task,&interpreter.path,FS_ACCESS_READ,false)<0)
        sched_task_mm(task)->nondumpable=true;
    ret=task_lookup_exec_guard(caller,&interpreter.path,&temporary);
    if(ret)goto done;
    ret=vfs_exec_source_open(&interpreter.path,&interp_source);
    if(ret)goto done;
    ret=elf64_read_headers(interp_source,&interp_image,&interp_len);
    if(ret){if(ret==-8)ret=-80;goto done;}
    if(!elf64_probe_image(interp_image,interp_len,vfs_exec_source_node(interp_source)->size,true,&interp_info) ||
       interp_info.abi_major!=main_info.abi_major){ret=-80;goto done;}
    interp_bias=elf64_choose_bias(&interp_info,true);
    if(!interp_bias || interp_info.entry>UINT64_MAX-interp_bias){ret=-8;goto done;}
    ret=elf64_map_one(task,interp_source,interp_image,&interp_info,interp_bias,"interpreter");
    if(ret)goto done;
    task->dynamic_launch.main_base=main_info.load_bias;
    task->dynamic_launch.main_entry=main_info.entry;
    task->dynamic_launch.main_phdr=main_info.phdr_vaddr;
    task->dynamic_launch.interp_base=interp_bias;
    task->dynamic_launch.interp_entry=interp_info.entry+interp_bias;
    task->dynamic_launch.abi_major=main_info.abi_major;
    for(uint32_t i=0;i+1<sizeof(task->dynamic_launch.main_path) && task->path[i];++i) {
        task->dynamic_launch.main_path[i]=task->path[i];task->dynamic_launch.main_path[i+1]=0;
    }
    main_info.interpreter_entry=interp_info.entry+interp_bias;*out=main_info;
done:
    vfs_exec_source_put(interp_source);
    if(temporary){storage_inode_allow_write(temporary);(void)storage_inode_put(temporary);}
    vfs_path_put(&interpreter.path);vfs_exec_source_put(main_source);
    return ret;
}
