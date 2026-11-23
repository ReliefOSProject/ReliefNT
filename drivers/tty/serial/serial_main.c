#include <reliefos/driver.h>

#include <reliefnt/serial.h>
#include <linux/errno.h>

static int serial_is_ready(void)
{
    return serial_com1_available();
}

static void serial_write(const char *text)
{
    if (text) serial_tx_record(text, __builtin_strlen(text), true);
}

static int serial_driver_init(const struct reliefos_driver_kernel_api *api)
{
    static const struct reliefos_driver_serial_ops ops = {
        .is_ready = serial_is_ready,
        .write = serial_write,
    };
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < sizeof(*api)) {
        return -22;
    }
    if (!serial_com1_available()) return -ENODEV;
    return api->register_serial(&ops);
}

const struct reliefos_driver_module serial_driver_module = {
    .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_module),
    .kind = RELIEFOS_DRIVER_KIND_SERIAL,
    .name = "serial",
    .version = 1U,
    .reserved = 0,
    .init = serial_driver_init,
    .fini = NULL,
};
