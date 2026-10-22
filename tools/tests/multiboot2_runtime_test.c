#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/console.h>
#include <reliefnt/multiboot2.h>

static _Alignas(8) unsigned char bytes[4096];
static size_t used;
static unsigned checks;

/** @brief Discard diagnostics; the test owns no kernel console or locks. */
void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}

/** @brief Begin a host-owned, eight-byte-aligned Multiboot2 fixture. */
static void reset(void)
{
    memset(bytes, 0, sizeof(bytes));
    used = 8;
}

/** @brief Append a sized tag; return its writable fixture-owned payload. */
static unsigned char *tag(uint32_t type, uint32_t size)
{
    assert(size >= 8 && used + size + 8 < sizeof(bytes));
    struct multiboot2_tag header = {type, size};
    memcpy(bytes + used, &header, sizeof(header));
    unsigned char *payload = bytes + used + 8;
    used += (size + 7u) & ~7u;
    return payload;
}

/** @brief Finish the fixture with a standard end tag and total byte length. */
static void finish(void)
{
    (void)tag(0, 8);
    uint32_t total = (uint32_t)used;
    memcpy(bytes, &total, sizeof(total));
}

/** @brief Assert a clean error result without any partially published state. */
static void bad(uint32_t magic, uintptr_t address)
{
    struct boot_info out, zero = {0};
    memset(&out, 0xa5, sizeof(out));
    assert(multiboot2_parse(magic, address, &out) == -EINVAL);
    assert(memcmp(&out, &zero, sizeof(out)) == 0);
    ++checks;
}

/** @brief Test BIOS/EFI fields and reject malformed tags using actual code. */
int main(void)
{
    const uint32_t magic = MULTIBOOT2_BOOTLOADER_MAGIC;
    struct boot_info out;
    reset(); finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(out.magic == magic && out.module_count == 0 && out.efi_system_table == 0);
    ++checks;
    bad(0, (uintptr_t)bytes);
    bad(magic, 0);
    bad(magic, UINTPTR_MAX - 3);
    bad(magic, (uintptr_t)bytes + 1);
    assert(multiboot2_parse(magic, (uintptr_t)bytes, NULL) == -EINVAL);
    ++checks;

    reset();
    memcpy(tag(1, 15), "root=/", 7);
    struct multiboot2_tag_mmap *map = (void *)(tag(6, 40) - 8);
    map->entry_size = 24;
    map->entries[0] = (struct multiboot2_mmap_entry){0x100000, 0x200000, 1, 0};
    struct multiboot2_tag_framebuffer *fb = (void *)(tag(8, 38) - 8);
    fb->framebuffer_addr = 0xe0000000;
    fb->framebuffer_pitch = 4096;
    fb->framebuffer_width = 1024;
    fb->framebuffer_height = 768;
    fb->framebuffer_bpp = 32;
    fb->framebuffer_type = 1;
    const unsigned char rgb[] = {16, 8, 8, 8, 0, 8};
    memcpy((unsigned char *)fb + sizeof(*fb), rgb, sizeof(rgb));
    unsigned char *new_rsdp = tag(15, 44);
    (void)tag(14, 28); /* The newer RSDP must win even when ordered first. */
    (void)tag(0xf00d, 16);
    finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(!strcmp(out.cmdline, "root=/") && out.mmap_entry_count == 1);
    assert(out.mmap_entry_size == 24 && out.rsdp_addr == (uintptr_t)new_rsdp);
    assert(out.framebuffer_addr == 0xe0000000 && out.framebuffer_width == 1024);
    assert(out.framebuffer_red_field_position == 16 && out.framebuffer_blue_mask_size == 8);
    ++checks;
    map->entry_size = 0; bad(magic, (uintptr_t)bytes);
    map->entry_size = 8; bad(magic, (uintptr_t)bytes);
    map->entry_size = 25; bad(magic, (uintptr_t)bytes);
    map->entry_size = 24;
    fb->size = 32; bad(magic, (uintptr_t)bytes); fb->size = 38;

    reset();
    map = (void *)(tag(6, 16 + 2 * 28) - 8);
    map->entry_size = 28;
    const struct multiboot2_mmap_entry extended = {0x100000, 0x200000, 1, 0};
    memcpy(map->entries, &extended, sizeof(extended));
    memcpy((unsigned char *)map->entries + 28, &extended, sizeof(extended));
    finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(out.mmap_entry_size == 28 && out.mmap_entry_count == 2);
    ++checks;

    reset();
    uint64_t table = 0x12345678000ULL;
    memcpy(tag(12, 16), &table, sizeof(table));
    struct multiboot2_tag_efi_mmap *efi = (void *)(tag(17, 56) - 8);
    efi->descr_size = 40; efi->descr_vers = 1;
    finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(out.efi_system_table == table && out.efi_mmap_entry_count == 1);
    ++checks;
    efi->descr_size = 0; bad(magic, (uintptr_t)bytes);
    efi->descr_size = 16; bad(magic, (uintptr_t)bytes);
    efi->descr_size = 41; bad(magic, (uintptr_t)bytes);

    reset();
    efi = (void *)(tag(17, 16 + 2 * 44) - 8);
    efi->descr_size = 44;
    const struct efi_memory_descriptor descriptor = {.physical_start = 0x100000,
                                                     .number_of_pages = 512};
    memcpy(efi->efi_mmap, &descriptor, sizeof(descriptor));
    memcpy(efi->efi_mmap + 44, &descriptor, sizeof(descriptor));
    finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(out.efi_mmap_entry_size == 44 && out.efi_mmap_entry_count == 2);
    ++checks;

    reset(); (void)tag(1, 10); bytes[16] = bytes[17] = 'x'; finish();
    bad(magic, (uintptr_t)bytes);
    reset();
    struct multiboot2_tag_module *mod = (void *)(tag(3, 17) - 8);
    mod->mod_start = 0x1000; mod->mod_end = 0x2000;
    finish();
    assert(multiboot2_parse(magic, (uintptr_t)bytes, &out) == 0);
    assert(out.module_count == 1 && out.modules[0].start == 0x1000);
    ++checks;
    mod->mod_end = 0x800; bad(magic, (uintptr_t)bytes);
    mod->mod_end = 0x2000; mod->string[0] = 'x'; bad(magic, (uintptr_t)bytes);
    reset();
    for (unsigned i = 0; i < 17; ++i) {
        mod = (void *)(tag(3, 17) - 8);
        mod->mod_start = 0x1000; mod->mod_end = 0x2000;
    }
    finish(); bad(magic, (uintptr_t)bytes);

    reset(); finish();
    struct multiboot2_tag *end = (void *)(bytes + 8);
    end->size = 0; bad(magic, (uintptr_t)bytes);
    end->size = 7; bad(magic, (uintptr_t)bytes);
    end->size = 24; bad(magic, (uintptr_t)bytes);
    end->size = 8; end->type = 0xff; bad(magic, (uintptr_t)bytes);
    uint32_t *total = (void *)bytes;
    *total = 8; bad(magic, (uintptr_t)bytes);

    reset(); finish();
    void *high = mmap((void *)0x1000000000ULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(high != MAP_FAILED && (uintptr_t)high > UINT32_MAX);
    memcpy(high, bytes, used);
    assert(multiboot2_parse(magic, (uintptr_t)high, &out) == 0);
    assert(out.multiboot_info == (uintptr_t)high);
    assert(munmap(high, 4096) == 0);
    ++checks;
    printf("PASS Multiboot2: %u valid/malformed BIOS/EFI, bounded tags, ACPI preference and full-width pointer cases\n", checks);
    return 0;
}
