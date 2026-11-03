/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <reliefnt/paging.h>
#include <reliefnt/smp.h>

static uint32_t cpu;
static unsigned queries[7];
static const uint8_t widths[] = {48, 36, 0, 31, 53, 32, 52};

/** @brief Select the fixture CPU without executing platform instructions.
 * @return Current logical CPU identity, including the uncached invalid case.
 */
uint32_t smp_current_cpu(void) { return cpu; }

/** @brief Model immutable CPUID leaves for seven distinct CPU capabilities.
 * @param leaf Extended CPU leaf requested by production paging code.
 * @return Maximum leaf or physical/virtual width encoding; no mapping data.
 */
uint32_t paging_test_extended_cpuid_eax(uint32_t leaf)
{
    unsigned index = cpu < 7 ? cpu : 0;
    ++queries[index];
    if (leaf == 0x80000000u)
        return index == 2 ? 0x80000001u : 0x80000008u;
    assert(leaf == 0x80000008u);
    return (48u << 8) | widths[index];
}

/** @brief Catch repeated hardware probing, cross-CPU reuse and widened bounds.
 * @return Zero after production physical-range decisions and cached probes pass.
 */
int main(void)
{
    static const uint8_t expected[] = {48, 36, 0, 0, 0, 32, 52};
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        for (cpu = 0; cpu < 7; ++cpu) {
            assert(paging_physical_bits() == expected[cpu]);
            if (expected[cpu]) {
                uint64_t limit = 1ULL << expected[cpu];
                assert(paging_phys_addressable(limit - 1, 1));
                assert(!paging_phys_addressable(limit - 1, 2));
                assert(!paging_phys_addressable(limit, 0));
                assert(!paging_phys_addressable(UINT64_MAX, 2));
            } else {
                assert(!paging_phys_addressable(0, 1));
            }
        }
    }
    for (cpu = 0; cpu < 7; ++cpu)
        assert(queries[cpu] == (cpu == 2 ? 1u : 2u));
    cpu = SMP_MAX_CPUS;
    assert(paging_physical_bits() == 48);
    assert(paging_physical_bits() == 48);
    assert(queries[0] == 6);
    puts("physical width: cached per CPU, unsupported cached, range bounds retained: PASS");
    return 0;
}
