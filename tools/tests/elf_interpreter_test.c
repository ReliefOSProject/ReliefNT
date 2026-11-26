#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/exec/elf.c"

static int elf_test_random_status;

int kernel_random_fill(void *buffer, size_t length)
{
    if (elf_test_random_status) return elf_test_random_status;
    memset(buffer, 0, length);
    return 0;
}

static void test_large_pie_bias(void)
{
    struct elf_image_info main_image = {
        .dynamic = true, .low_vaddr = 0, .high_vaddr = 0x0d700000ULL,
    };
    uint64_t bias = elf64_choose_bias(&main_image, false);
    uint64_t start = bias + main_image.low_vaddr;
    uint64_t end = bias + main_image.high_vaddr;
    assert(bias >= ELF_DYN_MAIN_MIN);
    assert(end <= ELF_DYN_MAIN_MAX);
    assert((bias & 0x1fffffULL) == 0);
    assert(!(start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START));
    assert(end < RELIEFNT_USER_TOP);

    struct elf_image_info high_low_image = {
        .dynamic = true, .low_vaddr = ELF_DYN_MAIN_MIN + 0x400000ULL,
        .high_vaddr = ELF_DYN_MAIN_MIN + 0x800000ULL,
    };
    uint64_t high_low_bias = elf64_choose_bias(&high_low_image, false);
    assert(high_low_bias != 0);
    assert(high_low_bias + high_low_image.low_vaddr >= ELF_DYN_MAIN_MIN);
    assert(high_low_bias + high_low_image.high_vaddr <= ELF_DYN_MAIN_MAX);

    struct elf_image_info interpreter = {
        .dynamic = true, .low_vaddr = 0, .high_vaddr = 0x00300000ULL,
    };
    uint64_t interpreter_bias = elf64_choose_bias(&interpreter, true);
    assert(interpreter_bias >= ELF_DYN_INTERP_MIN);
    assert(interpreter_bias + interpreter.high_vaddr <= ELF_DYN_INTERP_MAX);

    struct elf_image_info oversized = {
        .dynamic = true, .low_vaddr = 0,
        .high_vaddr = ELF_DYN_MAIN_MAX - ELF_DYN_MAIN_MIN + PAGE_SIZE,
    };
    assert(elf64_choose_bias(&oversized, false) == 0);
    elf_test_random_status = -1;
    assert(elf64_choose_bias(&interpreter, true) == 0);
    elf_test_random_status = 0;
}

int main(void)
{
    test_large_pie_bias();
    unsigned char image[4096] = {0};
    struct elf64_ehdr *eh = (void *)image;
    memcpy(eh->e_ident, "\177ELF\2\1\1", 7);
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = 1;
    eh->e_entry = 0x400300;
    eh->e_ehsize = sizeof(*eh);
    eh->e_phoff = sizeof(*eh);
    eh->e_phentsize = sizeof(struct elf64_phdr);
    eh->e_phnum = 3;
    struct elf64_phdr *ph = (void *)(image + eh->e_phoff);
    ph[0] = (struct elf64_phdr){.p_type = PT_LOAD, .p_flags = 4u | PF_X,
        .p_vaddr = 0x400000, .p_filesz = sizeof(image), .p_memsz = sizeof(image), .p_align = 4096};
    const char path[] = "/lib/ld-musl-x86_64.so.1";
    memcpy(image + 512, path, sizeof(path));
    ph[1] = (struct elf64_phdr){.p_type = PT_INTERP, .p_offset = 512, .p_filesz = sizeof(path)};
    ph[2].p_type = PT_DYNAMIC;
    struct elf_image_info info;
    assert(elf64_probe(image, sizeof(image), &info));
    assert(!info.dynamic && !strcmp(info.interp, path));
    assert(info.phdr_vaddr == 0x400000 + sizeof(*eh));
    eh->e_type = ET_DYN;
    assert(elf64_probe(image, sizeof(image), &info));
    assert(info.dynamic && !strcmp(info.interp, path));
    eh->e_type = ET_EXEC;
    image[512 + sizeof(path) - 1] = 'x';
    assert(!elf64_probe(image, sizeof(image), &info));
    ph[1].p_type = 0;
    ph[2].p_type = 0;
    assert(elf64_probe(image, sizeof(image), &info) && !info.interp[0]);
    puts("PASS ET_EXEC/PT_INTERP, PIE, static ELF and malformed interpreter");
}
