#include <assert.h>
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <reliefnt/paging.h>

/**
 * @brief Compare the production CPU width query with the host's extended CPUID leaf.
 * @return Zero when unsupported or valid widths agree; assertion failure otherwise.
 */
int main(void)
{
    unsigned a, b, c, d;
    unsigned maximum = __get_cpuid_max(0x80000000u, NULL);
    uint8_t expected = 0;
    if (maximum >= 0x80000008u && __get_cpuid(0x80000008u, &a, &b, &c, &d)) {
        unsigned width = a & 0xffu;
        if (width >= 32 && width <= 52) expected = (uint8_t)width;
    }
    assert(paging_physical_bits() == expected);
    puts("production physical width matches host CPUID: PASS");
    return 0;
}
