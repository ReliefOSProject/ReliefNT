#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/device/registry.c"

static unsigned init_calls;

static int fixture_init(const struct reliefos_driver_kernel_api *api)
{
    (void)api;
    ++init_calls;
    return 0;
}

int main(void)
{
    struct reliefos_driver_module legacy = {
        .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
        .abi_version = 1U,
        .struct_size = sizeof(struct reliefos_driver_module),
        .name = "legacy-v1",
        .init = fixture_init,
    };
    struct reliefos_driver_module current = legacy;
    current.abi_version = RELIEFOS_DRIVER_ABI_VERSION;
    memcpy(current.name, "current-v2", sizeof("current-v2"));

    assert(!driver_module_abi_valid(&legacy));
    assert(init_calls == 0);
    assert(driver_module_abi_valid(&current));
    assert(init_calls == 0);
    puts("PASS driver ABI v1 is rejected before module init; v2 descriptor accepted");
    return 0;
}
