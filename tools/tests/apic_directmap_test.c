#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#define RELIEFNT_APIC_TEST 1
#include "../../arch/x86_64/interrupt/apic.c"

static uint64_t aliases[2];
static uint64_t cache_phys[2];
static unsigned cache_calls;
static bool cache_ok = true;

void *mm_phys_to_virt(uint64_t phys)
{
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (cache_phys[i] == phys) return (void *)(uintptr_t)aliases[i];
    }
    return NULL;
}

bool paging_mmio_uncached(uint64_t phys, uint64_t len)
{
    assert(len == 4096);
    for (unsigned i = 0; i < sizeof(cache_phys) / sizeof(cache_phys[0]); ++i) {
        if (cache_phys[i] == phys) {
            ++cache_calls;
            return cache_ok;
        }
    }
    return false;
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

static void reset_fixture(void)
{
    local_apic_present = true;
    local_apic_enabled = true;
    local_apic_phys = 0xfee00000ULL;
    local_apic_base = (uintptr_t)local_apic_phys;
    ioapic_present = true;
    ioapic_count = 1;
    ioapics[0] = (struct ioapic_info){
        .base = (volatile uint32_t *)(uintptr_t)0xfec00000ULL,
        .phys = 0xfec00000ULL,
    };
    cache_phys[0] = local_apic_phys;
    cache_phys[1] = ioapics[0].phys;
    aliases[0] = 0xffffff8000000000ULL + cache_phys[0];
    aliases[1] = 0xffffff8000000000ULL + cache_phys[1];
    cache_calls = 0;
    cache_ok = true;
}

static void test_successful_rebind(void)
{
    reset_fixture();
    apic_map_kernel_aliases();
    assert(cache_calls == 2);
    assert(local_apic_base == aliases[0]);
    assert(ioapics[0].base == (volatile uint32_t *)(uintptr_t)aliases[1]);
    assert(local_apic_present && local_apic_enabled && ioapic_present);
}

static void test_failed_rebind_disables_only_unmapped_controllers(void)
{
    reset_fixture();
    cache_ok = false;
    apic_map_kernel_aliases();
    assert(cache_calls == 2);
    assert(!local_apic_present && !local_apic_enabled && local_apic_base == 0);
    assert(!ioapic_present && ioapics[0].base == NULL);
}

int main(void)
{
    test_successful_rebind();
    test_failed_rebind_disables_only_unmapped_controllers();
    puts("PASS APIC MMIO: UC/NX direct-map rebind and safe disable on failure");
    return 0;
}
