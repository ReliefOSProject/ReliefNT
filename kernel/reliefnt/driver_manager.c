/*
 * ReliefOS kernel driver manager: owns built-in and loadable driver lifecycle.
 * Provides registration, probing, initialization, and device event dispatch.
 */
#include <reliefnt/console.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/e1000.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/input.h>
#include <reliefnt/mm.h>
#include <reliefnt/net.h>
#include <reliefnt/pci.h>
#include <reliefnt/pci_irq.h>
#include <reliefnt/driver_resources.h>
#include <reliefnt/audio.h>
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/lock.h>
#include <reliefnt/panic.h>
#include <reliefnt/storage.h>
#include <reliefnt/time.h>
#include <reliefos/layout.h>

#include "../arch/x86_64/port.h"

#define EARLY_SERIAL_COM1 0x3f8u
#define EARLY_SERIAL_LSR  (EARLY_SERIAL_COM1 + 5u)

/* Kernel diagnostics must be available before serial.drv is loaded.  Keep a
 * tiny COM1 backend here; the loadable driver later replaces it through
 * register_serial(), without changing the public console API. */
static int early_serial_ready;

static void early_serial_putc(char ch)
{
    uint32_t attempts = 100000u;
    if (!early_serial_ready) {
        return;
    }
    while (!(x86_64_inb((uint16_t)EARLY_SERIAL_LSR) & 0x20u) && attempts--) {
        __asm__ volatile("pause");
    }
    x86_64_outb((uint8_t)ch, (uint16_t)EARLY_SERIAL_COM1);
}

static void early_serial_write(const char *text)
{
    while (text && *text) {
        if (*text == '\n') {
            early_serial_putc('\r');
        }
        early_serial_putc(*text++);
    }
}

#define DRIVER_DIRECTORY RELIEFOS_LAYOUT_RELIEFOS_DRIVERS
#define DRIVER_CONFIG_PATH RELIEFOS_PATH_DRIVERS_CONF
#define DRIVER_CONFIG_CAP 1024U
#define DRIVER_ELF_MAX_SECTIONS 64U
#define DRIVER_ELF_MAX_IMAGE (4U * 1024U * 1024U)

#define ELF_ET_REL 1U
#define ELF_EM_X86_64 62U
#define ELF_SHT_PROGBITS 1U
#define ELF_SHT_SYMTAB 2U
#define ELF_SHT_STRTAB 3U
#define ELF_SHT_RELA 4U
#define ELF_SHT_NOBITS 8U
#define ELF_SHF_ALLOC 0x2ULL
#define ELF_SHN_UNDEF 0U
#define ELF_SHN_ABS 0xfff1U
#define ELF_R_X86_64_64 1U
#define ELF_R_X86_64_PC32 2U
#define ELF_R_X86_64_PLT32 4U
#define ELF_R_X86_64_32 10U
#define ELF_R_X86_64_32S 11U

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct elf64_sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

struct driver_slot {
    struct reliefos_driver_info info;
    const struct reliefos_driver_module *module;
    uint64_t image_phys;
    uint32_t image_pages;
    int cleanup_error;
};

static struct driver_slot driver_slots[RELIEFOS_DRIVER_MAX];
static int32_t loading_slot = -1;
static int32_t cleanup_slot = -1;
static const struct reliefos_driver_mouse_ops *mouse_ops;
static bool mouse_visible = true;
static const struct reliefos_driver_serial_ops *serial_ops;
static const struct reliefos_driver_e1000_ops *e1000_ops;
static const struct reliefos_driver_audio_ops *audio_ops;
static uint32_t mouse_owner;
static uint32_t serial_owner;
static uint32_t e1000_owner;
static uint32_t audio_owner;
static uint32_t audio_generation, audio_lease_generation;
static struct mouse_state mouse_cache;
static uint8_t e1000_empty_mac[6];
static uint8_t e1000_mac_cache[6];
static struct kernel_spinlock manager_service_lock = KERNEL_SPINLOCK_INIT;
static struct kernel_spinlock e1000_mac_cache_lock = KERNEL_SPINLOCK_INIT;

/** @brief Resume manager ownership after a phase that may wait or sleep.
 * @param scope Active manager wait scope whose token must be consumed.
 * @return None. A lost transaction is a kernel invariant failure; no slot state
 * or module resources may be reclaimed without the resumed transaction.
 */
static void driver_manager_wait_end_or_panic(struct driver_manager_wait_scope *scope)
{
    if (driver_manager_wait_end(scope) < 0)
        panic("driver manager failed to resume execution ownership");
}

/**
 * @brief Copy src into dst up to cap-1 bytes and always NUL-terminate; safe with NULL src or cap 0.
 */
static void driver_copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t pos = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[pos] && pos + 1U < cap) {
        dst[pos] = src[pos];
        ++pos;
    }
    dst[pos] = 0;
}

/**
 * @brief Return 1 when left and right are equal NUL-terminated strings (NULL never equals).
 */
static int driver_text_equal(const char *left, const char *right)
{
    uint32_t index = 0;
    while (left && right && left[index] == right[index]) {
        if (left[index] == 0) {
            return 1;
        }
        ++index;
    }
    return 0;
}

/**
 * @brief Compare a string embedded in a buffer (bounded by available bytes) with right; the NUL must fit.
 */
static int driver_elf_string_equal(const char *left, uint64_t available,
                                   const char *right)
{
    uint64_t index = 0;
    while (right && right[index]) {
        if (index >= available || !left || left[index] != right[index]) {
            return 0;
        }
        ++index;
    }
    return right && index < available && left[index] == 0;
}

/**
 * @brief Return 1 when text begins with the whole of prefix (NULL-safe).
 */
static int driver_text_starts_with(const char *text, const char *prefix)
{
    uint32_t index = 0;
    while (text && prefix && prefix[index]) {
        if (text[index] != prefix[index]) {
            return 0;
        }
        ++index;
    }
    return prefix && prefix[index] == 0;
}

/**
 * @brief Length of a NUL-terminated string, never reading past cap bytes.
 */
static uint32_t driver_text_length(const char *text, uint32_t cap)
{
    uint32_t length = 0;
    while (text && length < cap && text[length]) {
        ++length;
    }
    return length;
}

/**
 * @brief Return 1 when name ends with the ".drv" extension.
 */
static int driver_has_drv_suffix(const char *name)
{
    uint32_t length = driver_text_length(name, RELIEFOS_DRIVER_FILE_LEN);
    return length > 4U && name[length - 4U] == '.' &&
           name[length - 3U] == 'd' && name[length - 2U] == 'r' &&
           name[length - 1U] == 'v';
}

/**
 * @brief True for a ".drv" name of legal length using only [A-Za-z0-9._-].
 */
static int driver_file_name_valid(const char *file)
{
    uint32_t length = driver_text_length(file, RELIEFOS_DRIVER_FILE_LEN);
    if (!driver_has_drv_suffix(file) || length == 0 || length >= RELIEFOS_DRIVER_FILE_LEN) {
        return 0;
    }
    for (uint32_t index = 0; index < length; ++index) {
        char ch = file[index];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Case-sensitive ordering compare (negative/zero/positive); NULL is treated as unequal.
 */
static int driver_name_compare(const char *left, const char *right)
{
    uint32_t index = 0;
    while (left && right && left[index] == right[index]) {
        if (left[index] == 0) {
            return 0;
        }
        ++index;
    }
    if (!left || !right) {
        return left ? 1 : (right ? -1 : 0);
    }
    return (uint8_t)left[index] < (uint8_t)right[index] ? -1 : 1;
}

/**
 * @brief Sort comparator that forces serial.drv ahead of everything, then alphabetical order.
 */
static int driver_load_order_compare(const char *left, const char *right)
{
    int left_is_serial = driver_text_equal(left, "serial.drv");
    int right_is_serial = driver_text_equal(right, "serial.drv");
    if (left_is_serial != right_is_serial) {
        return left_is_serial ? -1 : 1;
    }
    return driver_name_compare(left, right);
}

/**
 * @brief Build "/usr/lib/reliefos/drivers/<file>" into dst, clamped to cap bytes and NUL-terminated.
 */
static void driver_make_path(char *dst, uint32_t cap, const char *file)
{
    uint32_t pos = 0;
    const char *prefix = DRIVER_DIRECTORY "/";
    if (!dst || cap == 0) {
        return;
    }
    while (prefix[pos] && pos + 1U < cap) {
        dst[pos] = prefix[pos];
        ++pos;
    }
    for (uint32_t index = 0; file && file[index] && pos + 1U < cap; ++index) {
        dst[pos++] = file[index];
    }
    dst[pos] = 0;
}

/**
 * @brief Mark the slot failed, record the error text, and log it only when status is negative.
 */
static void driver_set_error(struct driver_slot *slot, int status, const char *error)
{
    if (!slot) {
        return;
    }
    slot->info.state = RELIEFOS_DRIVER_STATE_FAILED;
    driver_copy_text(slot->info.error, sizeof(slot->info.error), error);
    if (status < 0) {
        console_printf("[driver] %s failed status=%d: %s\n", slot->info.file,
                       status, slot->info.error);
    }
}

/**
 * @brief Round value up to a multiple of alignment, clamping alignment to 1..4096.
 */
static uint64_t driver_align_up(uint64_t value, uint64_t alignment)
{
    if (alignment < 1U) {
        alignment = 1U;
    }
    if (alignment > 4096U) {
        alignment = 4096U;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

/**
 * @brief Return 1 when [offset, offset+size) fits inside total without overflow.
 */
static int driver_range_valid(uint64_t offset, uint64_t size, uint64_t total)
{
    return offset <= total && size <= total - offset;
}

/**
 * @brief Zero size bytes starting at address.
 */
static void driver_memzero(void *address, uint64_t size)
{
    uint8_t *bytes = (uint8_t *)address;
    while (size--) {
        *bytes++ = 0;
    }
}

/**
 * @brief Copy size bytes from src to dst.
 */
static void driver_memcpy(void *dst, const void *src, uint64_t size)
{
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    while (size--) {
        *out++ = *in++;
    }
}

/**
 * @brief Return 1 when drivers.conf contains a "disabled=<file>" line for this driver.
 */
static int driver_config_disabled(const char *file)
{
    struct storage_node node;
    char config[DRIVER_CONFIG_CAP];
    uint32_t got = 0;
    uint32_t pos = 0;
    if (!file || storage_lookup_path(DRIVER_CONFIG_PATH, &node) < 0 ||
        node.type != RELIEFOS_FS_TYPE_FILE || node.size == 0) {
        return 0;
    }
    if (storage_read_node(&node, 0, config,
                          node.size >= sizeof(config) ? sizeof(config) - 1U : (uint32_t)node.size,
                          &got) < 0) {
        return 0;
    }
    config[got < sizeof(config) ? got : sizeof(config) - 1U] = 0;
    while (pos < got) {
        uint32_t start = pos;
        while (pos < got && config[pos] != '\n' && config[pos] != '\r') {
            ++pos;
        }
        if (pos > start && driver_text_starts_with(config + start, "disabled=") &&
            driver_text_equal(config + start + 9U, file)) {
            return 1;
        }
        while (pos < got && (config[pos] == '\n' || config[pos] == '\r')) {
            ++pos;
        }
    }
    return 0;
}

/**
 * @brief Rebuild drivers.conf: a version header plus one "disabled=" line per disabled driver.
 */
static int driver_write_config(void)
{
    char config[DRIVER_CONFIG_CAP];
    uint32_t pos = 0;
    const char *header = "version=1\n";
    for (uint32_t index = 0; header[index] && pos + 1U < sizeof(config); ++index) {
        config[pos++] = header[index];
    }
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        const struct driver_slot *slot = &driver_slots[index];
        const char *prefix = "disabled=";
        if (!slot->info.file[0] || !(slot->info.flags & RELIEFOS_DRIVER_FLAG_DISABLED)) {
            continue;
        }
        for (uint32_t j = 0; prefix[j] && pos + 1U < sizeof(config); ++j) {
            config[pos++] = prefix[j];
        }
        for (uint32_t j = 0; slot->info.file[j] && pos + 1U < sizeof(config); ++j) {
            config[pos++] = slot->info.file[j];
        }
        if (pos + 1U >= sizeof(config)) {
            return -28;
        }
        config[pos++] = '\n';
    }
    config[pos] = 0;
    return storage_write_file(DRIVER_CONFIG_PATH, config, pos);
}

/**
 * @brief Return the driver slot whose file name matches, or NULL when absent.
 */
static struct driver_slot *driver_find_file(const char *file)
{
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        if (driver_slots[index].info.file[0] &&
            driver_text_equal(driver_slots[index].info.file, file)) {
            return &driver_slots[index];
        }
    }
    return 0;
}

/**
 * @brief Find a slot by file, or claim a free one and seed its disabled/autostart state from drivers.conf.
 */
static struct driver_slot *driver_get_slot(const char *file)
{
    struct driver_slot *slot = driver_find_file(file);
    if (slot) {
        return slot;
    }
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        slot = &driver_slots[index];
        if (!slot->info.file[0]) {
            driver_memzero(slot, sizeof(*slot));
            slot->info.id = index;
            slot->info.state = driver_config_disabled(file)
                                   ? RELIEFOS_DRIVER_STATE_DISABLED
                                   : RELIEFOS_DRIVER_STATE_UNLOADED;
            slot->info.flags = RELIEFOS_DRIVER_FLAG_AUTOSTART |
                               (slot->info.state == RELIEFOS_DRIVER_STATE_DISABLED
                                    ? RELIEFOS_DRIVER_FLAG_DISABLED : 0U);
            driver_copy_text(slot->info.file, sizeof(slot->info.file), file);
            return slot;
        }
    }
    return 0;
}

/**
 * @brief Read the whole file into freshly allocated memory and return its bytes and length.
 */
static int driver_read_file(const char *path, const uint8_t **out_data, uint64_t *out_len)
{
    const void *data = 0;
    size_t length = 0;
    int ret = storage_read_file(path, &data, &length);
    if (ret < 0 || !data || length == 0) {
        return ret < 0 ? ret : -5;
    }
    *out_data = (const uint8_t *)data;
    *out_len = (uint64_t)length;
    return 0;
}

/**
 * @brief Release the pages that driver_read_file allocated for a file's contents.
 */
static void driver_release_file(const void *data, uint64_t length)
{
    uint32_t pages = (uint32_t)((length + 4095U) / 4096U);
    if (data && pages) {
        mm_free_pages((uint64_t)(uintptr_t)data, pages);
    }
}

/**
 * @brief Resolve a symbol to its runtime address: its own value if absolute, else its section base plus value.
 */
static int driver_symbol_value(const struct elf64_sym *symbol, uint16_t shnum,
                               const struct elf64_shdr *sections,
                               const uint64_t section_addresses[], uint64_t *out)
{
    if (!symbol || !out) {
        return -22;
    }
    if (symbol->shndx == ELF_SHN_ABS) {
        *out = symbol->value;
        return 0;
    }
    if (symbol->shndx == ELF_SHN_UNDEF || symbol->shndx >= shnum ||
        !sections || symbol->value > sections[symbol->shndx].size ||
        !section_addresses[symbol->shndx]) {
        return -8;
    }
    *out = section_addresses[symbol->shndx] + symbol->value;
    return 0;
}

/**
 * @brief Patch every RELA relocation in the loaded image; -8 on malformed data, -95 on an unknown type.
 * @param data Borrowed validated module bytes.
 * @param length Size of the source image.
 * @param header Validated ELF header.
 * @param sections Borrowed section header table.
 * @param section_addresses Loaded writable section addresses.
 * @return 0 after byte-safe relocation stores, or a negative format/type error.
 */
static int driver_apply_relocations(const uint8_t *data, uint64_t length,
                                    const struct elf64_ehdr *header,
                                    const struct elf64_shdr *sections,
                                    const uint64_t section_addresses[])
{
    for (uint16_t index = 0; index < header->shnum; ++index) {
        const struct elf64_shdr *rela_section = &sections[index];
        const struct elf64_shdr *target;
        const struct elf64_shdr *symbols;
        const struct elf64_shdr *strings;
        uint64_t count;
        if (rela_section->type != ELF_SHT_RELA) {
            continue;
        }
        if (rela_section->info >= header->shnum || rela_section->link >= header->shnum ||
            rela_section->entsize != sizeof(struct elf64_rela) ||
            !driver_range_valid(rela_section->offset, rela_section->size, length)) {
            return -8;
        }
        target = &sections[rela_section->info];
        symbols = &sections[rela_section->link];
        if (symbols->type != ELF_SHT_SYMTAB || symbols->link >= header->shnum ||
            symbols->entsize != sizeof(struct elf64_sym) ||
            !driver_range_valid(symbols->offset, symbols->size, length) ||
            !section_addresses[rela_section->info]) {
            return -8;
        }
        strings = &sections[symbols->link];
        if (strings->type != ELF_SHT_STRTAB ||
            !driver_range_valid(strings->offset, strings->size, length)) {
            return -8;
        }
        count = rela_section->size / sizeof(struct elf64_rela);
        for (uint64_t rel_index = 0; rel_index < count; ++rel_index) {
            const struct elf64_rela *rela =
                (const struct elf64_rela *)(data + rela_section->offset +
                                            rel_index * sizeof(struct elf64_rela));
            uint32_t symbol_index = (uint32_t)(rela->info >> 32);
            uint32_t type = (uint32_t)rela->info;
            uint64_t symbol_count = symbols->size / sizeof(struct elf64_sym);
            const struct elf64_sym *symbol;
            uint64_t symbol_value;
            uint64_t patch;
            int64_t value;
            if (symbol_index >= symbol_count || rela->offset >= target->size ||
                !driver_range_valid(rela->offset, type == ELF_R_X86_64_64 ? 8U : 4U,
                                    target->size)) {
                return -8;
            }
            symbol = (const struct elf64_sym *)(data + symbols->offset +
                                                symbol_index * sizeof(struct elf64_sym));
            if (driver_symbol_value(symbol, header->shnum, sections, section_addresses,
                                    &symbol_value) < 0) {
                return -8;
            }
            patch = section_addresses[rela_section->info] + rela->offset;
            if (type == ELF_R_X86_64_64) {
                uint64_t word = symbol_value + (uint64_t)rela->addend;
                driver_memcpy((void *)(uintptr_t)patch, &word, sizeof(word));
            } else if (type == ELF_R_X86_64_PC32 || type == ELF_R_X86_64_PLT32) {
                value = (int64_t)symbol_value + rela->addend - (int64_t)patch;
                if (value < -2147483648LL || value > 2147483647LL) {
                    return -8;
                }
                uint32_t word = (uint32_t)value;
                driver_memcpy((void *)(uintptr_t)patch, &word, sizeof(word));
            } else if (type == ELF_R_X86_64_32 || type == ELF_R_X86_64_32S) {
                value = (int64_t)symbol_value + rela->addend;
                if ((type == ELF_R_X86_64_32 &&
                     (value < 0 || (uint64_t)value > 0xffffffffULL)) ||
                    (type == ELF_R_X86_64_32S &&
                     (value < -2147483648LL || value > 2147483647LL))) {
                    return -8;
                }
                uint32_t word = (uint32_t)value;
                driver_memcpy((void *)(uintptr_t)patch, &word, sizeof(word));
            } else {
                return -95;
            }
        }
    }
    return 0;
}

/**
 * @brief Locate the exported reliefos_driver_module symbol and resolve it to a pointer in the loaded image.
 * Transitional: old driver ELFs export the legacy leonos_driver_module name,
 * so both spellings are accepted until the old ecosystem is retired.
 */
static int driver_find_module(const uint8_t *data, uint64_t length,
                              const struct elf64_ehdr *header,
                              const struct elf64_shdr *sections,
                              const uint64_t section_addresses[],
                              const struct reliefos_driver_module **out)
{
    for (uint16_t index = 0; index < header->shnum; ++index) {
        const struct elf64_shdr *symbols = &sections[index];
        const struct elf64_shdr *strings;
        uint64_t count;
        if (symbols->type != ELF_SHT_SYMTAB || symbols->link >= header->shnum ||
            symbols->entsize != sizeof(struct elf64_sym) ||
            !driver_range_valid(symbols->offset, symbols->size, length)) {
            continue;
        }
        strings = &sections[symbols->link];
        if (strings->type != ELF_SHT_STRTAB ||
            !driver_range_valid(strings->offset, strings->size, length)) {
            return -8;
        }
        count = symbols->size / sizeof(struct elf64_sym);
        for (uint64_t symbol_index = 0; symbol_index < count; ++symbol_index) {
            const struct elf64_sym *symbol =
                (const struct elf64_sym *)(data + symbols->offset +
                                            symbol_index * sizeof(struct elf64_sym));
            uint64_t value;
            const char *name;
            if (symbol->name >= strings->size) {
                return -8;
            }
            name = (const char *)(data + strings->offset + symbol->name);
            if (!driver_range_valid(symbols->offset + symbol_index * sizeof(struct elf64_sym),
                                    sizeof(struct elf64_sym), length) ||
                (!driver_elf_string_equal(name, strings->size - symbol->name,
                                          "reliefos_driver_module") &&
                 !driver_elf_string_equal(name, strings->size - symbol->name,
                                          "leonos_driver_module"))) {
                continue;
            }
            if (symbol->shndx == ELF_SHN_UNDEF || symbol->shndx >= header->shnum ||
                symbol->value > sections[symbol->shndx].size ||
                sizeof(struct reliefos_driver_module) >
                    sections[symbol->shndx].size - symbol->value ||
                driver_symbol_value(symbol, header->shnum, sections, section_addresses,
                                    &value) < 0) {
                return -8;
            }
            *out = (const struct reliefos_driver_module *)(uintptr_t)value;
            return 0;
        }
    }
    return -2;
}

/**
 * @brief Bind the currently-loading driver's mouse ops as the active mouse backend.
 */
static int driver_register_mouse(const struct reliefos_driver_mouse_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->poll || !ops->get_state) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    mouse_ops = ops;
    mouse_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the currently-loading driver's serial ops as the active serial backend.
 */
static int driver_register_serial(const struct reliefos_driver_serial_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->write) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    serial_ops = ops;
    serial_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the currently-loading driver's e1000 ops as the active network backend.
 */
static int driver_register_e1000(const struct reliefos_driver_e1000_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->mac || !ops->send ||
        !ops->poll || !ops->get_info) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    e1000_ops = ops;
    e1000_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the loading driver's v1 backend with a non-reused generation.
 * @param ops Borrowed validated callbacks, owned by the loaded image.
 * @return Zero, -EINVAL or -ENOSPC. Task init under manager admission.
 */
static int driver_register_audio(const struct reliefos_driver_audio_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->configure ||
        !ops->write || !ops->get_state) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (audio_generation == UINT32_MAX) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        return -28;
    }
    ++audio_generation;
    audio_ops = ops;
    audio_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Capture and pin one published mouse callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_mouse_ops *driver_mouse_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_mouse_ops *ops = mouse_ops;
    uint32_t owner = mouse_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published serial callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_serial_ops *driver_serial_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_serial_ops *ops = serial_ops;
    uint32_t owner = serial_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published e1000 callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_e1000_ops *driver_e1000_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_e1000_ops *ops = e1000_ops;
    uint32_t owner = e1000_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published audio callback table with its matching owner.
 * @param generation Exact OSS lease generation, or zero for native compatibility.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_audio_ops *driver_audio_service_pin_bound(uint32_t generation,
                                                                             uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_audio_ops *ops = audio_ops;
    uint32_t owner = audio_owner;
    if (!ops || (generation && (generation != audio_generation ||
                                generation != audio_lease_generation)) ||
        !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Read a byte from an I/O port (exposed to drivers through the ABI).
 */
static uint8_t driver_api_inb(uint16_t port)
{
    return x86_64_inb(port);
}

/**
 * @brief Write a byte to an I/O port (exposed to drivers through the ABI).
 */
static void driver_api_outb(uint16_t port, uint8_t value)
{
    x86_64_outb(value, port);
}

/**
 * @brief Read a 32-bit value from an I/O port (exposed to drivers through the ABI).
 */
static uint32_t driver_api_inl(uint16_t port)
{
    return x86_64_inl(port);
}

/**
 * @brief Write a 32-bit value to an I/O port (exposed to drivers through the ABI).
 */
static void driver_api_outl(uint16_t port, uint32_t value)
{
    x86_64_outl(value, port);
}

/**
 * @brief Report the framebuffer's width and height, or -2 when no framebuffer is available.
 */
static int driver_api_framebuffer_size(uint32_t *width, uint32_t *height)
{
    const struct framebuffer *framebuffer = framebuffer_get();
    if (!framebuffer || !framebuffer->available) {
        return -2;
    }
    if (width) {
        *width = framebuffer->width;
    }
    if (height) {
        *height = framebuffer->height;
    }
    return 0;
}

/**
 * @brief Find a PCI device by vendor/device id and copy its identity into the driver-ABI struct.
 */
static int driver_api_pci_find(uint16_t vendor_id, uint16_t device_id,
                               struct reliefos_driver_pci_device *out)
{
    struct pci_device device;
    int ret = pci_find_device(vendor_id, device_id, &device);
    if (ret < 0 || !out) {
        return ret < 0 ? ret : -22;
    }
    *out = (struct reliefos_driver_pci_device){
        .bus = device.bus,
        .slot = device.slot,
        .function = device.function,
        .class_code = device.class_code,
        .vendor_id = device.vendor_id,
        .device_id = device.device_id,
        .subclass = device.subclass,
        .prog_if = device.prog_if,
        .header_type = 0,
        .reserved = 0,
    };
    return 0;
}

/** @brief Allocate a DMA lease for the loading module.
 * @param pages Requested pages.
 * @param mask Inclusive final-byte DMA mask.
 * @return Owned physical run or zero. Init/task context; core records owner.
 */
static uint64_t driver_alloc_dma(uint32_t pages,uint64_t mask)
{
    return driver_alloc_dma_owned((uint32_t)loading_slot,pages,mask);
}
/** @brief Map a BAR lease for the loading module.
 * @param phys Aligned BAR base of a quiesced function.
 * @param bytes Requested byte length.
 * @return Owned mapping pointer or NULL. Init/task context, execution transaction.
 */
static void *driver_map_mmio(uint64_t phys,uint64_t bytes)
{
    return driver_map_mmio_owned((uint32_t)loading_slot,phys,bytes);
}
/** @brief Register an IRQ lease for the loading module.
 * @param dev Borrowed function identity.
 * @param handler IRQ-safe W1C callback, never sleeping.
 * @param opaque Module-owned context retained through synchronization.
 * @param out_handle Receives generation token.
 * @return 0 or negative errno. Init/task context; no callback under ledger lock.
 */
static int driver_request_pci_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *),void *opaque,uint32_t *out_handle)
{
    return driver_request_pci_irq_owned((uint32_t)loading_slot,dev,handler,opaque,out_handle);
}
/** @brief Copy a v2 card contract and bind the currently loading module owner.
 * @param identity Borrowed identity, copied by core.
 * @param ops Borrowed operations, copied by core.
 * @param opaque Context remains module owned until unload synchronization.
 * @param out_id Receives generation card token.
 * @return 0 or negative errno. Task context during init; no registry lock held.
 */
static int driver_audio_register_card(const struct audio_card_identity *identity,
    const struct audio_card_ops *ops,void *opaque,uint32_t *out_id)
{
    if(loading_slot<0)return -22;
    return audio_register_card_owned(identity,ops,opaque,(uint32_t)loading_slot,out_id);
}

/** @brief Record the first unsafe teardown result in the serialized fini phase.
 * @param error Negative errno; nonnegative or out-of-phase reports are ignored.
 * @return None. No resource is released and no lock or wait is performed.
 */
static void driver_report_teardown_failure(int error)
{
    if (error < 0 && cleanup_slot >= 0 &&
        cleanup_slot < (int32_t)RELIEFOS_DRIVER_MAX &&
        !driver_slots[cleanup_slot].cleanup_error)
        driver_slots[cleanup_slot].cleanup_error = error;
}

/** @brief Invoke one module fini and collect its append-only failure report.
 * @param slot Owner slot under the manager admission gate, execution suspended.
 * @param module Live descriptor retained throughout this synchronous callback.
 * @return 0 for safe automatic cleanup or the first reported negative errno.
 */
static int driver_run_fini(struct driver_slot *slot,
                            const struct reliefos_driver_module *module)
{
    slot->cleanup_error = 0;
    cleanup_slot = (int32_t)slot->info.id;
    if (module->fini) module->fini();
    cleanup_slot = -1;
    return slot->cleanup_error;
}

static const struct reliefos_driver_kernel_api driver_kernel_api = {
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_kernel_api),
    .inb = driver_api_inb,
    .outb = driver_api_outb,
    .inl = driver_api_inl,
    .outl = driver_api_outl,
    .alloc_pages = mm_alloc_pages,
    .free_pages = mm_free_pages,
    .console_write = console_write,
    .input_push_mouse = input_push_mouse,
    .input_push_mouse_wheel = input_push_mouse_wheel,
    .framebuffer_size = driver_api_framebuffer_size,
    .pci_find = driver_api_pci_find,
    .pci_read16 = pci_config_read16,
    .pci_write16 = pci_config_write16,
    .pci_read32 = pci_config_read32,
    .ticks = time_ticks,
    .sleep_ms = time_sleep_ms,
    .register_mouse = driver_register_mouse,
    .register_serial = driver_register_serial,
    .register_e1000 = driver_register_e1000,
    .register_audio = driver_register_audio,
    .pci_enumerate = pci_enumerate,
    .map_mmio = driver_map_mmio,
    .unmap_mmio = driver_unmap_mmio,
    .alloc_dma = driver_alloc_dma,
    .free_dma = driver_free_dma,
    .request_pci_irq = driver_request_pci_irq,
    .free_pci_irq = driver_free_pci_irq,
    .audio_register_card = driver_audio_register_card,
    .audio_unregister_card = audio_unregister_card,
    .audio_period_elapsed = audio_period_elapsed,
    .audio_control_changed = audio_control_changed,
    .pci_write32 = pci_config_write32,
    .audio_set_service = audio_card_set_service,
    .report_teardown_failure = driver_report_teardown_failure,
    .audio_request_disconnect = audio_request_controller_disconnect,
};

/**
 * @brief Detach any device ops owned by slot_id and reset their state (notifying net on e1000).
 */
static void driver_clear_services(uint32_t slot_id)
{
    bool detach_network = false;
    driver_manager_service_disable(slot_id);
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (mouse_ops && mouse_owner == slot_id) {
        mouse_ops = 0;
        mouse_owner = 0;
        mouse_cache = (struct mouse_state){0};
    }
    if (serial_ops && serial_owner == slot_id) {
        serial_ops = 0;
        serial_owner = 0;
    }
    if (e1000_ops && e1000_owner == slot_id) {
        detach_network = true;
        e1000_ops = 0;
        e1000_owner = 0;
    }
    if (audio_ops && audio_owner == slot_id) {
        audio_ops = 0;
        audio_owner = 0;
    }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    if (detach_network) net_driver_detached();
}

/** @brief Reject a copied module before it can disturb an owned PCI function.
 * @param candidate Validated, borrowed descriptor in the new module image.
 * @return True when a loaded or retained module has the same bounded name.
 * Task context under manager admission; no callback, allocation or PCI access.
 */
static bool driver_module_name_loaded(const struct reliefos_driver_module *candidate)
{
    for (uint32_t slot = 0; slot < RELIEFOS_DRIVER_MAX; ++slot) {
        const struct driver_slot *existing = &driver_slots[slot];
        if (existing->info.state != RELIEFOS_DRIVER_STATE_LOADED || !existing->module)
            continue;
        uint32_t n;
        for (n = 0; n < RELIEFOS_DRIVER_NAME_LEN; ++n) {
            if (existing->module->name[n] != candidate->name[n]) break;
            if (!candidate->name[n]) return true;
        }
        if (n == RELIEFOS_DRIVER_NAME_LEN) return true;
    }
    return false;
}

/**
 * @brief Load ELF, initialize module, and roll back owner resources on failure.
 * @param slot Manager-owned slot under serialized task transaction.
 * @return 0 or negative errno. Task context, no registry lock during callbacks;
 * retains image if DMA STOP or fini fails; cancels service/IRQ before fini.
 */
static int driver_load_slot(struct driver_slot *slot)
{
    char path[RELIEFOS_DRIVER_FILE_LEN + 16U];
    const uint8_t *data = 0;
    uint64_t length = 0;
    const struct elf64_ehdr *header;
    const struct elf64_shdr *sections;
    uint64_t section_addresses[DRIVER_ELF_MAX_SECTIONS];
    uint64_t image_size = 0;
    uint64_t image_phys = 0;
    uint32_t image_pages = 0;
    const struct reliefos_driver_module *module = 0;
    int ret = -5;
    if (!slot || slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) {
        return slot ? 0 : -22;
    }
    if (slot->info.flags & RELIEFOS_DRIVER_FLAG_DISABLED) {
        return -13;
    }
    driver_make_path(path, sizeof(path), slot->info.file);
    slot->info.state = RELIEFOS_DRIVER_STATE_LOADING;
    slot->info.error[0] = 0;
    if ((ret = driver_read_file(path, &data, &length)) < 0) {
        driver_set_error(slot, ret, "Cannot read module file");
        return ret;
    }
    if (length < sizeof(*header)) {
        ret = -8;
        driver_set_error(slot, ret, "ELF header is truncated");
        goto out;
    }
    header = (const struct elf64_ehdr *)data;
    if (header->ident[0] != 0x7f || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2U || header->ident[5] != 1U ||
        header->type != ELF_ET_REL || header->machine != ELF_EM_X86_64 ||
        header->ehsize != sizeof(*header) || header->shnum == 0 ||
        header->shnum > DRIVER_ELF_MAX_SECTIONS ||
        header->shentsize != sizeof(struct elf64_shdr) ||
        !driver_range_valid(header->shoff,
                            (uint64_t)header->shnum * sizeof(struct elf64_shdr), length)) {
        ret = -8;
        driver_set_error(slot, ret, "Unsupported ELF64 relocatable module");
        goto out;
    }
    sections = (const struct elf64_shdr *)(data + header->shoff);
    driver_memzero(section_addresses, sizeof(section_addresses));
    for (uint16_t index = 0; index < header->shnum; ++index) {
        const struct elf64_shdr *section = &sections[index];
        if (section->type != ELF_SHT_NOBITS &&
            !driver_range_valid(section->offset, section->size, length)) {
            ret = -8;
            driver_set_error(slot, ret, "ELF section exceeds module file");
            goto out;
        }
        if (!(section->flags & ELF_SHF_ALLOC)) {
            continue;
        }
        image_size = driver_align_up(image_size, section->addralign);
        if (section->size > DRIVER_ELF_MAX_IMAGE || image_size > DRIVER_ELF_MAX_IMAGE - section->size) {
            ret = -12;
            driver_set_error(slot, ret, "Module image is too large");
            goto out;
        }
        image_size += section->size;
    }
    if (image_size == 0) {
        ret = -8;
        driver_set_error(slot, ret, "Module has no allocatable sections");
        goto out;
    }
    image_pages = (uint32_t)((image_size + 4095U) / 4096U);
    image_phys = mm_alloc_pages(image_pages);
    if (!image_phys) {
        ret = -12;
        driver_set_error(slot, ret, "No memory for module image");
        goto out;
    }
    driver_memzero((void *)(uintptr_t)image_phys, (uint64_t)image_pages * 4096U);
    image_size = 0;
    for (uint16_t index = 0; index < header->shnum; ++index) {
        const struct elf64_shdr *section = &sections[index];
        if (!(section->flags & ELF_SHF_ALLOC)) {
            continue;
        }
        image_size = driver_align_up(image_size, section->addralign);
        section_addresses[index] = image_phys + image_size;
        if (section->type != ELF_SHT_NOBITS && section->size) {
            driver_memcpy((void *)(uintptr_t)section_addresses[index],
                          data + section->offset, section->size);
        }
        image_size += section->size;
    }
    if ((ret = driver_apply_relocations(data, length, header, sections, section_addresses)) < 0) {
        driver_set_error(slot, ret, "Unsupported or invalid module relocation");
        goto out;
    }
    if ((ret = driver_find_module(data, length, header, sections, section_addresses, &module)) < 0 ||
        !module || module->magic != RELIEFOS_DRIVER_MODULE_MAGIC ||
        module->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        module->struct_size != sizeof(*module) || !module->name[0] || !module->init) {
        ret = ret < 0 ? ret : -8;
        driver_set_error(slot, ret, "Driver descriptor ABI is invalid");
        goto out;
    }
    if (driver_module_name_loaded(module)) {
        ret = -16;
        driver_set_error(slot, ret, "Module name already loaded");
        goto out;
    }
    loading_slot = (int32_t)slot->info.id;
    struct driver_manager_wait_scope init_scope = {0};
    if ((ret = driver_manager_wait_begin(&init_scope)) < 0) {
        loading_slot = -1;
        driver_set_error(slot, ret, "Cannot release execution transaction for init");
        goto out;
    }
    ret = module->init(&driver_kernel_api);
    driver_manager_wait_end_or_panic(&init_scope);
    loading_slot = -1;
    if (ret < 0) {
        struct driver_manager_wait_scope rollback_scope = {0};
        driver_manager_service_close(slot->info.id);
        if (driver_manager_wait_begin(&rollback_scope) < 0)
            panic("driver manager cannot suspend execution for init rollback");
        int disconnect=audio_unregister_owner(slot->info.id,1);
        if (disconnect) {
            /* A failed STOP forbids reclaiming code or DMA still owned by the
             * device. Preserve a retryable slot instead of freeing its image. */
            slot->module=module;slot->image_phys=image_phys;slot->image_pages=image_pages;
            slot->info.kind=module->kind;
            slot->info.load_address=image_phys;slot->info.image_size=image_size;
            slot->info.abi_version=module->abi_version;slot->info.version=module->version;
            driver_copy_text(slot->info.name,sizeof(slot->info.name),module->name);
            driver_set_error(slot,disconnect,"Audio STOP failed; module retained");
            slot->info.state=RELIEFOS_DRIVER_STATE_LOADED;
            image_phys=0;
            driver_manager_wait_end_or_panic(&rollback_scope);
            driver_manager_service_enable(slot->info.id);
            ret=disconnect;goto out;
        }
        driver_manager_service_drain(slot->info.id);
        driver_resources_release(slot->info.id,true);
        int cleanup = driver_run_fini(slot, module);
        driver_manager_wait_end_or_panic(&rollback_scope);
        if (cleanup < 0) {
            slot->module=module;slot->image_phys=image_phys;slot->image_pages=image_pages;
            slot->info.kind=module->kind;
            slot->info.load_address=image_phys;slot->info.image_size=image_size;
            slot->info.abi_version=module->abi_version;slot->info.version=module->version;
            driver_copy_text(slot->info.name,sizeof(slot->info.name),module->name);
            driver_set_error(slot,cleanup,"Unsafe teardown; module retained for retry");
            slot->info.state=RELIEFOS_DRIVER_STATE_LOADED;
            image_phys=0;ret=cleanup;goto out;
        }
        driver_clear_services(slot->info.id);
        driver_resources_release(slot->info.id,false);
        driver_set_error(slot, ret, "Driver initialization failed");
        goto out;
    }
    slot->module = module;
    slot->image_phys = image_phys;
    slot->image_pages = image_pages;
    slot->info.state = RELIEFOS_DRIVER_STATE_LOADED;
    slot->info.kind = module->kind;
    slot->info.abi_version = module->abi_version;
    slot->info.version = module->version;
    slot->info.load_address = image_phys;
    slot->info.image_size = image_size;
    driver_copy_text(slot->info.name, sizeof(slot->info.name), module->name);
    slot->info.error[0] = 0;
    driver_manager_service_enable(slot->info.id);
    console_printf("[driver] loaded %s as %s abi=%u\n", slot->info.file,
                   slot->info.name, slot->info.abi_version);
    ret = 0;
out:
    loading_slot = -1;
    driver_release_file(data, length);
    if (ret < 0 && image_phys) {
        mm_free_pages(image_phys, image_pages);
    }
    return ret;
}

/**
 * @brief Disconnect owned audio before fini, synchronize MSI, then release image.
 * @param slot Owned loaded module slot, serialized by manager transaction.
 * @param force Nonzero disconnects active leases; normal active audio is EBUSY.
 * @return 0 or negative errno. Task context. Module init/fini and task-context
 * callbacks may wait only in the released manager wait phase; ISR/service callbacks
 * must remain bounded and nonblocking and may not acquire the execution transaction.
 * Detached stream/DMA owners survive in PCM until release. A failed fini
 * retains DMA/MMIO/image with service admission closed; later unload retries.
 */
static int driver_unload_slot(struct driver_slot *slot, uint32_t force)
{
    if (!slot || slot->info.state != RELIEFOS_DRIVER_STATE_LOADED || !slot->module) {
        return -2;
    }
    if (!force && slot->info.kind == RELIEFOS_DRIVER_KIND_NETWORK) {
        return -16;
    }
    uint32_t owner = slot->info.id;
    driver_manager_service_close(owner);
    struct driver_manager_wait_scope unload_scope = {0};
    int ret = driver_manager_wait_begin(&unload_scope);
    if (ret < 0) {
        driver_manager_service_reopen(owner);
        return ret;
    }
    ret = audio_unregister_owner(owner,force);
    if (ret) {
        driver_manager_wait_end_or_panic(&unload_scope);
        driver_manager_service_reopen(owner);
        return ret;
    }
    driver_manager_service_drain(owner);
    driver_resources_release(owner,true);
    ret = driver_run_fini(slot, slot->module);
    driver_manager_wait_end_or_panic(&unload_scope);
    if (ret < 0) {
        driver_set_error(slot,ret,"Unsafe teardown; module retained for retry");
        slot->info.state=RELIEFOS_DRIVER_STATE_LOADED;
        return ret;
    }
    driver_clear_services(owner);
    driver_resources_release(owner,false);
    if (slot->image_phys && slot->image_pages) {
        mm_free_pages(slot->image_phys, slot->image_pages);
    }
    slot->module = 0;
    slot->image_phys = 0;
    slot->image_pages = 0;
    slot->info.state = (slot->info.flags & RELIEFOS_DRIVER_FLAG_DISABLED)
                           ? RELIEFOS_DRIVER_STATE_DISABLED
                           : RELIEFOS_DRIVER_STATE_UNLOADED;
    slot->info.load_address = 0;
    slot->info.image_size = 0;
    slot->info.error[0] = 0;
    console_printf("[driver] unloaded %s%s\n", slot->info.file,
                   force ? " (forced)" : "");
    return 0;
}

/**
 * @brief Enumerate /drivers, sort entries by load order, and register each valid file as a slot.
 */
static void driver_scan(void)
{
    struct reliefos_dir_entry entries[RELIEFOS_FS_MAX_ENTRIES];
    uint32_t count = 0;
    if (storage_list_dir(DRIVER_DIRECTORY, entries, RELIEFOS_FS_MAX_ENTRIES, &count) < 0) {
        console_printf("[driver] no %s directory\n", DRIVER_DIRECTORY);
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        for (uint32_t next = index + 1U; next < count; ++next) {
            if (driver_load_order_compare(entries[next].name, entries[index].name) < 0) {
                struct reliefos_dir_entry entry = entries[index];
                entries[index] = entries[next];
                entries[next] = entry;
            }
        }
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (entries[index].type == RELIEFOS_FS_TYPE_FILE &&
            driver_file_name_valid(entries[index].name)) {
            /* Register this driver file as a slot for later autoload. */
            (void)driver_get_slot(entries[index].name);
        }
    }
}

/**
 * @brief Clear all driver slots and the cached mouse state at startup.
 */
void driver_manager_init(void)
{
    driver_memzero(driver_slots, sizeof(driver_slots));
    mouse_cache = (struct mouse_state){0};
    console_printf("[driver] manager ready abi=%u\n", RELIEFOS_DRIVER_ABI_VERSION);
}

/** @brief Scan and load startup drivers with manager admission and execution ownership held.
 * @return None. Called only by the public boot hook or an already-admitted RESCAN action.
 */
static void driver_manager_autoload_admitted(void)
{
    driver_scan();
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        struct driver_slot *slot = &driver_slots[index];
        if (!slot->info.file[0] || slot->info.state == RELIEFOS_DRIVER_STATE_DISABLED ||
            slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) {
            continue;
        }
        if (driver_load_slot(slot) < 0) {
            if (slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) continue;
            console_printf("[driver] retrying %s\n", slot->info.file);
            /* Second load attempt after the first failed. */
            (void)driver_load_slot(slot);
        }
    }
}

/**
 * @brief Scan for drivers and load every autostart slot that is not disabled or already loaded.
 * @return None. Boot has no outer execution transaction, so this hook owns a
 * short manager transaction and suspends it only around waitable module phases.
 */
void driver_manager_autoload(void)
{
    if (driver_manager_phase_try_enter() < 0) {
        console_printf("[driver] autoload skipped: manager busy\n");
        return;
    }
    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    driver_manager_autoload_admitted();
    kernel_execution_unlock_irqrestore(execution_flags);
    driver_manager_phase_leave();
}

/**
 * @brief Fill the query with up to capacity driver infos and set the total count.
 */
int driver_manager_list(struct reliefos_driver_list *query)
{
    uint32_t count = 0;
    if (!query) {
        return -22;
    }
    if (driver_manager_phase_try_enter() < 0) return -16;
    if (query->capacity > RELIEFOS_DRIVER_MAX) {
        query->capacity = RELIEFOS_DRIVER_MAX;
    }
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        if (!driver_slots[index].info.file[0]) {
            continue;
        }
        if (query->drivers && count < query->capacity) {
            query->drivers[count] = driver_slots[index].info;
        }
        ++count;
    }
    query->count = count;
    driver_manager_phase_leave();
    return 0;
}

/**
 * @brief Dispatch a control action (rescan, load, unload, or enable/disable boot) on a driver.
 */
int driver_manager_control(struct reliefos_driver_control *request)
{
    struct driver_slot *slot;
    int ret;
    if (!request) {
        return -22;
    }
    ret = driver_manager_phase_try_enter();
    if (ret < 0) {
        request->status = ret;
        return ret;
    }
    if (request->action == RELIEFOS_DRIVER_CONTROL_RESCAN) {
        driver_manager_autoload_admitted();
        ret = 0;
        goto out;
    }
    if (!driver_file_name_valid(request->file)) {
        ret = -22;
        goto out;
    }
    slot = driver_get_slot(request->file);
    if (!slot) {
        ret = -28;
        goto out;
    }
    if (request->action == RELIEFOS_DRIVER_CONTROL_LOAD) {
        ret = driver_load_slot(slot);
    } else if (request->action == RELIEFOS_DRIVER_CONTROL_UNLOAD) {
        ret = driver_unload_slot(slot, 0);
    } else if (request->action == RELIEFOS_DRIVER_CONTROL_FORCE_UNLOAD) {
        ret = driver_unload_slot(slot, 1);
    } else if (request->action == RELIEFOS_DRIVER_CONTROL_ENABLE_BOOT ||
               request->action == RELIEFOS_DRIVER_CONTROL_DISABLE_BOOT) {
        uint32_t disable = request->action == RELIEFOS_DRIVER_CONTROL_DISABLE_BOOT;
        if (disable && slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) {
            ret = driver_unload_slot(slot, 1);
            if (ret < 0) {
                goto out;
            }
        }
        if (disable) {
            slot->info.flags |= RELIEFOS_DRIVER_FLAG_DISABLED;
            slot->info.state = RELIEFOS_DRIVER_STATE_DISABLED;
        } else {
            slot->info.flags &= ~RELIEFOS_DRIVER_FLAG_DISABLED;
            if (slot->info.state == RELIEFOS_DRIVER_STATE_DISABLED) {
                slot->info.state = RELIEFOS_DRIVER_STATE_UNLOADED;
            }
        }
        ret = driver_write_config();
    } else {
        ret = -22;
    }
out:
    request->status = ret;
    driver_manager_phase_leave();
    return ret;
}

/**
 * @brief Reserved startup hook; mouse state arrives through the registered driver.
 */
void mouse_init(void)
{
}

/**
 * @brief Poll the active mouse driver for new events, if one is registered.
 * @return None. IRQ dispatch may call this wrapper; it pins a consistent local
 * ops/owner snapshot across the callback without holding service locks. The
 * legacy poll callback must remain bounded and IRQ-safe.
 */
void mouse_poll(void)
{
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (ops && ops->poll) {
        ops->poll();
        driver_manager_service_unpin(owner);
    } else if (ops) {
        driver_manager_service_unpin(owner);
    }
}

/**
 * @brief Refresh the manager-owned mouse cache from a pinned driver snapshot.
 * @return Pointer to manager-owned cached state, never into the module image.
 */
const struct mouse_state *mouse_get_state(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (ops && ops->get_state) {
        state = (struct reliefos_driver_mouse_state){0};
        ops->get_state(&state);
        uint64_t flags;
        kernel_spin_lock_irqsave(&manager_service_lock, &flags);
        mouse_cache = (struct mouse_state){
            .x = state.x,
            .y = state.y,
            .buttons = state.buttons,
            .present = state.present != 0,
            .absolute = state.absolute != 0,
        };
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    }
    if (ops) driver_manager_service_unpin(owner);
    return &mouse_cache;
}

/**
 * @brief Record whether the mouse cursor should be drawn.
 */
void mouse_set_visible(bool visible)
{
    mouse_visible = visible;
}

/**
 * @brief Return the cached cursor visibility flag.
 */
bool mouse_is_visible(void)
{
    return mouse_visible;
}

/**
 * @brief Report the driver's pending-event counter, or 0 when no driver is registered.
 * @return Copied counter value; the matching ops/owner pin spans get_state.
 */
uint32_t mouse_event_count(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.event_count;
}

/**
 * @brief Report the driver's last PS/2 status byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_status(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_status;
}

/**
 * @brief Report the driver's last PS/2 data byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_data(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_data;
}

/**
 * @brief Report the driver's last PS/2 ack byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_ack(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_ack;
}

/**
 * @brief Copy the current cached mouse state into out.
 */
void driver_manager_mouse_state(struct mouse_state *out)
{
    if (out) {
        *out = *mouse_get_state();
    }
}

/**
 * @brief Forward mouse_event_count to callers outside this file.
 */
uint32_t driver_manager_mouse_event_count(void)
{
    return mouse_event_count();
}

/**
 * @brief Forward mouse_last_status to callers outside this file.
 */
uint8_t driver_manager_mouse_last_status(void)
{
    return mouse_last_status();
}

/**
 * @brief Forward mouse_last_data to callers outside this file.
 */
uint8_t driver_manager_mouse_last_data(void)
{
    return mouse_last_data();
}

/**
 * @brief Forward mouse_last_ack to callers outside this file.
 */
uint8_t driver_manager_mouse_last_ack(void)
{
    return mouse_last_ack();
}

/**
 * @brief Forward mouse_poll to callers outside this file.
 */
void driver_manager_mouse_poll(void)
{
    mouse_poll();
}

/**
 * @brief Program the COM1 UART (baud, frame format, FIFO, IRQ) and enable the early serial path.
 */
void serial_init(void)
{
    x86_64_outb(0x00u, (uint16_t)(EARLY_SERIAL_COM1 + 1u));
    x86_64_outb(0x80u, (uint16_t)(EARLY_SERIAL_COM1 + 3u));
    x86_64_outb(0x03u, (uint16_t)(EARLY_SERIAL_COM1 + 0u));
    x86_64_outb(0x00u, (uint16_t)(EARLY_SERIAL_COM1 + 1u));
    x86_64_outb(0x03u, (uint16_t)(EARLY_SERIAL_COM1 + 3u));
    x86_64_outb(0xc7u, (uint16_t)(EARLY_SERIAL_COM1 + 2u));
    x86_64_outb(0x0bu, (uint16_t)(EARLY_SERIAL_COM1 + 4u));
    early_serial_ready = 1;
}

/**
 * @brief Return 1 when a serial driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; one consistent ops/owner pin
 * spans is_ready, with no manager lock held during the callback.
 */
int serial_is_ready(void)
{
    uint32_t owner;
    const struct reliefos_driver_serial_ops *ops = driver_serial_service_pin(&owner);
    if (!ops) return 0;
    int ready = ops->is_ready ? ops->is_ready() : 0;
    driver_manager_service_unpin(owner);
    return ready;
}

/**
 * @brief Write text through the serial driver, or the built-in COM1 path before one loads.
 * @param text Kernel-owned NUL-terminated text borrowed through callback return.
 * @return None. The matching local ops/owner snapshot stays pinned across write.
 */
void serial_write(const char *text)
{
    uint32_t owner;
    const struct reliefos_driver_serial_ops *ops = driver_serial_service_pin(&owner);
    if (ops && ops->write) {
        ops->write(text);
        driver_manager_service_unpin(owner);
        return;
    }
    if (ops) driver_manager_service_unpin(owner);
    early_serial_write(text);
}

/**
 * @brief Reserved startup hook; the NIC is brought up by its driver's init().
 */
void e1000_init(void)
{
}

/**
 * @brief Return 1 when an e1000 driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; the matching ops/owner stays
 * pinned until is_ready returns.
 */
int e1000_is_ready(void)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return 0;
    int ready = ops->is_ready ? ops->is_ready() : 0;
    driver_manager_service_unpin(owner);
    return ready;
}

/**
 * @brief Return a kernel-owned copy of the NIC MAC, or a zeroed placeholder when absent.
 * @return Manager cache pointer; the module's borrowed pointer is copied while its
 * matching ops/owner snapshot remains pinned.
 */
const uint8_t *e1000_mac(void)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops || !ops->mac) {
        if (ops) driver_manager_service_unpin(owner);
        return e1000_empty_mac;
    }
    const uint8_t *borrowed = ops->mac();
    uint64_t flags;
    kernel_spin_lock_irqsave(&e1000_mac_cache_lock, &flags);
    if (borrowed) driver_memcpy(e1000_mac_cache, borrowed, sizeof(e1000_mac_cache));
    else driver_memzero(e1000_mac_cache, sizeof(e1000_mac_cache));
    kernel_spin_unlock_irqrestore(&e1000_mac_cache_lock, flags);
    driver_manager_service_unpin(owner);
    return e1000_mac_cache;
}

/**
 * @brief Send an Ethernet frame through the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned frame borrowed through send callback return.
 * @param len Number of bytes available at frame.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and send.
 */
int e1000_send(const void *frame, uint32_t len)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->send && ops->is_ready()
                  ? ops->send(frame, len) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Poll received frames from the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned receive buffer.
 * @param capacity Bytes available at frame.
 * @param out_len Receives the copied frame length when supplied by the driver.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and poll.
 */
int e1000_poll(void *frame, uint32_t capacity, uint32_t *out_len)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->poll && ops->is_ready()
                  ? ops->poll(frame, capacity, out_len) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Copy NIC identity independently of carrier state.
 * @param info Receives kernel-owned identity fields and MAC bytes.
 * @return None. Driver output is copied to a stack snapshot before unpin.
 */
void e1000_get_info(struct e1000_info *info)
{
    struct reliefos_driver_e1000_info source;
    if (!info) {
        return;
    }
    *info = (struct e1000_info){0};
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops || !ops->get_info) {
        if (ops) driver_manager_service_unpin(owner);
        return;
    }
    ops->get_info(&source);
    driver_manager_service_unpin(owner);
    info->present = source.present;
    info->active = source.active;
    info->vendor_id = source.vendor_id;
    info->device_id = source.device_id;
    info->bus = source.bus;
    info->slot = source.slot;
    info->function = source.function;
    for (uint32_t index = 0; index < 6; ++index) {
        info->mac[index] = source.mac[index];
    }
}

/**
 * @brief Configure the audio output format on the active driver, or -19 when absent.
 * @param format Kernel-owned format borrowed through configure callback return.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and configure.
 */
int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{ return driver_manager_audio_configure_bound(0, format); }

/** @brief Configure one generation-pinned v1 audio callback table.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param format Borrowed sample format.
 * @return Driver result or -ENODEV. Task context; no manager lock spans callback.
 */
int driver_manager_audio_configure_bound(uint32_t generation, const struct reliefos_audio_format *format)
{
    if (!format) {
        return -19;
    }
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->configure && ops->is_ready()
                  ? ops->configure(format) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Write audio samples; returns -19 and a NO_DEVICE status when no driver is ready.
 * @param data Kernel-owned sample bytes borrowed through write callback return.
 * @param length Number of valid sample bytes.
 * @param out_status Optional kernel-owned status destination.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and write.
 */
long driver_manager_audio_write(const void *data, uint32_t length,
                                uint32_t *out_status)
{ return driver_manager_audio_write_bound(0, data, length, out_status); }

/** @brief Write bytes through the exact acquired backend generation.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param data Borrowed samples. @param length Byte count.
 * @param out_status Optional status output.
 * @return Written bytes or errno. Task context; no manager lock spans callback.
 */
long driver_manager_audio_write_bound(uint32_t generation, const void *data, uint32_t length,
                                      uint32_t *out_status)
{
    if (out_status) {
        *out_status = RELIEFOS_AUDIO_STATUS_NO_DEVICE;
    }
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) {
        return -19;
    }
    long ret = ops->is_ready && ops->write && ops->is_ready()
                   ? ops->write(data, length, out_status) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Copy the audio driver's state into out, zeroing it when no driver is ready.
 * @param out Optional kernel-owned destination.
 * @return None. A consistent local ops/owner pin stays live through get_state.
 */
void driver_manager_audio_get_state(struct reliefos_audio_state *out)
{ if (out) (void)driver_manager_audio_state_bound(0, out); }

/** @brief Copy state while holding the exact backend's callback pin.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param out Borrowed output, cleared before attempting admission.
 * @return Zero or -ENODEV/-EINVAL. Task context; callback has no manager lock.
 */
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *out)
{
    if (!out) return -22;
    *out = (struct reliefos_audio_state){0};
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) return -19;
    if (ops->is_ready && ops->is_ready() && ops->get_state) ops->get_state(out);
    driver_manager_service_unpin(owner);
    return out->present && out->active ? 0 : -19;
}

/** @brief Reserve one OSS description on the current published v1 backend.
 * @param generation Receives a non-reused token on success.
 * @param state Receives real hardware state under a callback pin.
 * @return Zero, -EINVAL, -ENODEV or -EBUSY. Task context; no lock spans callback.
 */
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state)
{
    if (!generation || !state) return -22;
    *state = (struct reliefos_audio_state){0};
    uint64_t flags;uint32_t owner, token;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_audio_ops *ops = audio_ops;
    owner = audio_owner;token = audio_generation;
    if (!ops || !driver_manager_service_try_pin(owner)) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);return -19;
    }
    if (audio_lease_generation == token) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        driver_manager_service_unpin(owner);return -16;
    }
    audio_lease_generation = token;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    if (ops->is_ready() && ops->get_state) ops->get_state(state);
    driver_manager_service_unpin(owner);
    if (!state->present || !state->active) {
        driver_manager_audio_release(token);return -19;
    }
    *generation = token;return 0;
}

/** @brief Release only the lease generation held by the closing OSS description.
 * @param generation Nonzero acquired token, possibly stale after unload.
 * @return None. Task context; no callback or hardware access.
 */
void driver_manager_audio_release(uint32_t generation)
{
    uint64_t flags;kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (generation && generation == audio_lease_generation) audio_lease_generation = 0;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
}
