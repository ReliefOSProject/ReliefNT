/* Decode the standard Multiboot2 handoff before allocation or interrupts. */
#include <reliefnt/multiboot2.h>
#include <linux/errno.h>

/** @brief Test for a NUL within a borrowed tag payload, without reading padding. */
static bool tag_string_valid(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (!data[i]) return true;
    }
    return false;
}

/**
 * @brief Decode a bounded handoff into caller-owned storage, publishing only on success.
 * @param magic Multiboot2 bootloader magic in EAX.
 * @param info_addr Borrowed, readable, eight-byte-aligned physical information block.
 * @param out Writable output that must not alias the information block.
 * @return 0 on success, -EINVAL on malformed or unrepresentable information.
 * Called by the BSP with IRQs disabled, before heap/locks exist. Pointer fields
 * borrow the bootloader block; its lifetime extends through memory reservation.
 */
int multiboot2_parse(uint32_t magic, uintptr_t info_addr, struct boot_info *out)
{
    if (!out) return -EINVAL;
    *out = (struct boot_info){0};
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || !info_addr ||
        (info_addr & 7u) || info_addr > UINTPTR_MAX - sizeof(struct multiboot2_info))
        return -EINVAL;
    const struct multiboot2_info *info = (const void *)info_addr;
    if (info->total_size < 16 || (info->total_size & 7u) ||
        info_addr > UINTPTR_MAX - info->total_size) return -EINVAL;
    uintptr_t cursor = info_addr + sizeof(*info);
    uintptr_t end = info_addr + info->total_size;
    struct boot_info parsed = {0};
    parsed.magic = magic;
    parsed.multiboot_info = info_addr;
    while (end - cursor >= sizeof(struct multiboot2_tag)) {
        const struct multiboot2_tag *tag = (const void *)cursor;
        size_t remaining = end - cursor;
        if (tag->size < sizeof(*tag) || tag->size > remaining) return -EINVAL;
        size_t padded = ((size_t)tag->size + 7u) & ~(size_t)7u;
        if (padded > remaining) return -EINVAL;
        const uint8_t *payload = (const uint8_t *)tag + sizeof(*tag);
        size_t payload_size = tag->size - sizeof(*tag);
        switch (tag->type) {
        case MULTIBOOT2_TAG_TYPE_END:
            if (tag->size != 8 || remaining != 8) return -EINVAL;
            *out = parsed;
            return 0;
        case MULTIBOOT2_TAG_TYPE_CMDLINE:
        case MULTIBOOT2_TAG_TYPE_BOOT_LOADER_NAME:
            if (!tag_string_valid(payload, payload_size)) return -EINVAL;
            if (tag->type == MULTIBOOT2_TAG_TYPE_CMDLINE) parsed.cmdline = (const char *)payload;
            else parsed.bootloader = (const char *)payload;
            break;
        case MULTIBOOT2_TAG_TYPE_MODULE: {
            const struct multiboot2_tag_module *mod = (const void *)tag;
            if (tag->size <= sizeof(*mod) || mod->mod_end < mod->mod_start ||
                !tag_string_valid((const uint8_t *)mod->string, tag->size - sizeof(*mod)) ||
                parsed.module_count >= sizeof(parsed.modules) / sizeof(parsed.modules[0]))
                return -EINVAL;
            parsed.modules[parsed.module_count++] = (struct boot_module){
                mod->mod_start, mod->mod_end, mod->string};
            break;
        }
        case MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO: {
            if (payload_size < 8) return -EINVAL;
            const uint32_t *mem = (const void *)payload;
            parsed.memory_lower_kib = mem[0];
            parsed.memory_upper_kib = mem[1];
            break;
        }
        case MULTIBOOT2_TAG_TYPE_MMAP: {
            const struct multiboot2_tag_mmap *map = (const void *)tag;
            if (tag->size < sizeof(*map) || map->entry_size < sizeof(struct multiboot2_mmap_entry) ||
                (tag->size - sizeof(*map)) % map->entry_size) return -EINVAL;
            uint32_t count = (tag->size - sizeof(*map)) / map->entry_size;
            for (uint32_t i = 0; i < count; ++i) {
                const struct multiboot2_mmap_entry *entry = (const void *)
                    ((const uint8_t *)map->entries + (size_t)i * map->entry_size);
                if (entry->len > UINT64_MAX - entry->addr) return -EINVAL;
            }
            parsed.mmap_addr = (uintptr_t)map->entries;
            parsed.mmap_entry_size = map->entry_size;
            parsed.mmap_entry_count = count;
            break;
        }
        case MULTIBOOT2_TAG_TYPE_FRAMEBUFFER: {
            const struct multiboot2_tag_framebuffer *fb = (const void *)tag;
            if (tag->size < sizeof(*fb)) return -EINVAL;
            parsed.framebuffer_addr = fb->framebuffer_addr;
            parsed.framebuffer_width = fb->framebuffer_width;
            parsed.framebuffer_height = fb->framebuffer_height;
            parsed.framebuffer_pitch = fb->framebuffer_pitch;
            parsed.framebuffer_bpp = fb->framebuffer_bpp;
            parsed.framebuffer_type = fb->framebuffer_type;
            if (fb->framebuffer_type == MULTIBOOT2_FRAMEBUFFER_TYPE_RGB) {
                if (tag->size < sizeof(*fb) + MULTIBOOT2_FRAMEBUFFER_RGB_INFO_SIZE) return -EINVAL;
                const uint8_t *rgb = (const uint8_t *)fb + sizeof(*fb);
                if (!fb->framebuffer_bpp || fb->framebuffer_bpp > 32) return -EINVAL;
                for (unsigned i = 0; i < 6; i += 2) {
                    if (!rgb[i+1] || (unsigned)rgb[i] + rgb[i+1] > fb->framebuffer_bpp)
                        return -EINVAL;
                }
                parsed.framebuffer_red_field_position = rgb[0];
                parsed.framebuffer_red_mask_size = rgb[1];
                parsed.framebuffer_green_field_position = rgb[2];
                parsed.framebuffer_green_mask_size = rgb[3];
                parsed.framebuffer_blue_field_position = rgb[4];
                parsed.framebuffer_blue_mask_size = rgb[5];
            }
            uint64_t span = (uint64_t)fb->framebuffer_pitch * fb->framebuffer_height;
            if (span > UINT64_MAX - fb->framebuffer_addr) return -EINVAL;
            break;
        }
        case MULTIBOOT2_TAG_TYPE_EFI64:
            if (payload_size < 8) return -EINVAL;
            parsed.efi_system_table = *(const uint64_t *)(const void *)payload;
            break;
        case MULTIBOOT2_TAG_TYPE_EFI_MMAP: {
            const struct multiboot2_tag_efi_mmap *map = (const void *)tag;
            if (tag->size < sizeof(*map) || map->descr_size < sizeof(struct efi_memory_descriptor) ||
                (tag->size - sizeof(*map)) % map->descr_size) return -EINVAL;
            uint32_t count = (tag->size - sizeof(*map)) / map->descr_size;
            for (uint32_t i = 0; i < count; ++i) {
                const struct efi_memory_descriptor *entry = (const void *)
                    (map->efi_mmap + (size_t)i * map->descr_size);
                if (entry->number_of_pages > (UINT64_MAX - entry->physical_start) / 4096u)
                    return -EINVAL;
            }
            parsed.efi_mmap_addr = (uintptr_t)map->efi_mmap;
            parsed.efi_mmap_entry_size = map->descr_size;
            parsed.efi_mmap_entry_count = count;
            break;
        }
        case MULTIBOOT2_TAG_TYPE_ACPI_OLD:
            if (payload_size < 20) return -EINVAL;
            if (!parsed.rsdp_addr) parsed.rsdp_addr = (uintptr_t)payload;
            break;
        case MULTIBOOT2_TAG_TYPE_ACPI_NEW:
            if (payload_size < 36) return -EINVAL;
            parsed.rsdp_addr = (uintptr_t)payload;
            break;
        default:
            /* Unknown standard extension tags are safe to skip by their size. */
            break;
        }
        cursor += padded;
    }
    return -EINVAL;
}
