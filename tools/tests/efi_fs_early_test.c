#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Exercise the real early EFI filesystem path using firmware-protocol fixtures. */
#include "../../firmware/efi_fs.c"

static unsigned service_calls;
static unsigned pool_frees;
static unsigned file_reads;
static unsigned file_closes;
static const char mock_contents[] = "theme=win95\n";
static struct efi_file_protocol mock_root;
static struct efi_file_protocol mock_file;
static struct efi_simple_file_system_protocol mock_fs;
static efi_handle_t mock_handles[1];

/** @brief Return a caller interrupt-state sentinel without changing host flags. */
uint64_t kernel_irq_save(void)
{
    return 0x202;
}

/** @brief Ignore the firmware wrapper's caller-state restoration in the host fixture.
 * @param flags Saved state returned by kernel_irq_save().
 * @return None.
 */
void kernel_irq_restore(uint64_t flags)
{
    (void)flags;
}

/** @brief Accept formatted early-kernel messages without firmware output.
 * @param level Message severity.
 * @param format Format string.
 * @return None.
 */
void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

/** @brief Return the one fake EFI filesystem handle and track the protocol call.
 * @param search_type Firmware search type.
 * @param protocol Requested protocol identifier.
 * @param search_key Optional search key.
 * @param no_handles Output handle count.
 * @param buffer Output firmware-owned handle array.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_locate_handle_buffer(
    uint64_t search_type, struct efi_guid *protocol, void *search_key,
    uint64_t *no_handles, efi_handle_t **buffer)
{
    (void)search_type;
    (void)protocol;
    (void)search_key;
    ++service_calls;
    *no_handles = 1;
    mock_handles[0] = &mock_fs;
    *buffer = mock_handles;
    return EFI_SUCCESS;
}

/** @brief Release the fake firmware-allocated handle list.
 * @param buffer Firmware-owned handle array.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_free_pool(void *buffer)
{
    assert(buffer == mock_handles);
    ++service_calls;
    ++pool_frees;
    return EFI_SUCCESS;
}

/** @brief Return a bounded mock EFI memory map with two descriptors.
 * @param memory_map_size Input capacity and output map byte length.
 * @param memory_map Caller-owned map destination.
 * @param map_key Output firmware map key.
 * @param descriptor_size Output descriptor stride.
 * @param descriptor_version Output descriptor version.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_get_memory_map(
    uint64_t *memory_map_size, void *memory_map, uint64_t *map_key,
    uint64_t *descriptor_size, uint32_t *descriptor_version)
{
    assert(*memory_map_size >= 16);
    ++service_calls;
    memset(memory_map, 0x5a, 16);
    *memory_map_size = 16;
    *map_key = 7;
    *descriptor_size = 8;
    *descriptor_version = 1;
    return EFI_SUCCESS;
}

/** @brief Resolve the fake Simple File System protocol handle.
 * @param handle Input EFI handle.
 * @param protocol Requested protocol identifier.
 * @param interface Output interface pointer.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_handle_protocol(
    efi_handle_t handle, struct efi_guid *protocol, void **interface)
{
    (void)protocol;
    assert(handle == &mock_fs);
    ++service_calls;
    *interface = &mock_fs;
    return EFI_SUCCESS;
}

/** @brief Open the fake filesystem volume root.
 * @param self Protocol instance.
 * @param root Output root file handle.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_open_volume(
    struct efi_simple_file_system_protocol *self,
    struct efi_file_protocol **root)
{
    assert(self == &mock_fs);
    ++service_calls;
    *root = &mock_root;
    return EFI_SUCCESS;
}

/** @brief Return the file handle for the theme path.
 * @param self Root file handle.
 * @param new_handle Output opened file handle.
 * @param file_name UTF-16 EFI path.
 * @param open_mode Requested access mode.
 * @param attributes File attributes.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_file_open(
    struct efi_file_protocol *self, struct efi_file_protocol **new_handle,
    uint16_t *file_name, uint64_t open_mode, uint64_t attributes)
{
    assert(self == &mock_root);
    assert(file_name[0] == '\\' && open_mode == EFI_FILE_MODE_READ && attributes == 0);
    ++service_calls;
    *new_handle = &mock_file;
    return EFI_SUCCESS;
}

/** @brief Track closing either the opened test file or root volume.
 * @param self File protocol handle.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_file_close(
    struct efi_file_protocol *self)
{
    assert(self == &mock_file || self == &mock_root);
    ++service_calls;
    ++file_closes;
    return EFI_SUCCESS;
}

/** @brief Return the fake theme file's metadata.
 * @param self Open file protocol.
 * @param information_type Requested EFI file information GUID.
 * @param buffer_size Input capacity and output metadata bytes.
 * @param buffer Writable metadata destination.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_file_get_info(
    struct efi_file_protocol *self, struct efi_guid *information_type,
    uint64_t *buffer_size, void *buffer)
{
    struct efi_file_info *info = buffer;
    (void)information_type;
    assert(self == &mock_file && *buffer_size >= sizeof(*info));
    ++service_calls;
    memset(buffer, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->file_size = sizeof(mock_contents) - 1u;
    *buffer_size = sizeof(*info);
    return EFI_SUCCESS;
}

/** @brief Copy the fake theme content to the caller-provided buffer.
 * @param self Open file protocol.
 * @param buffer_size Input byte capacity and output bytes copied.
 * @param buffer Caller-owned writable destination.
 * @return EFI success status.
 */
static efi_status_t __attribute__((ms_abi)) mock_file_read(
    struct efi_file_protocol *self, uint64_t *buffer_size, void *buffer)
{
    size_t expected = sizeof(mock_contents) - 1u;
    assert(self == &mock_file && *buffer_size >= expected);
    ++service_calls;
    ++file_reads;
    memcpy(buffer, mock_contents, expected);
    *buffer_size = expected;
    return EFI_SUCCESS;
}

/** @brief Test bounded early reads, pool cleanup, and sealing against late EFI calls.
 * @return Process status; assertions report any lifecycle regression.
 */
int main(void)
{
    struct efi_boot_services services = {0};
    struct efi_system_table system_table = {0};
    char buffer[64] = {0};
    size_t length = 0;
    uint64_t entry_size = 0;
    uint32_t entry_count = 0;
    uint8_t memory_map[64] = {0};
    unsigned calls_after_shutdown;
    unsigned calls_after_final_map;

    services.locate_handle_buffer = mock_locate_handle_buffer;
    services.handle_protocol = mock_handle_protocol;
    services.free_pool = mock_free_pool;
    services.get_memory_map = mock_get_memory_map;
    system_table.boot_services = &services;
    mock_fs.open_volume = mock_open_volume;
    mock_root.open = mock_file_open;
    mock_root.close = mock_file_close;
    mock_file.close = mock_file_close;
    mock_file.get_info = mock_file_get_info;
    mock_file.read = mock_file_read;

    efi_fs_init((uint64_t)(uintptr_t)&system_table);
    assert(efi_fs_ready());
    assert(pool_frees == 1);
    assert(efi_fs_read_file_into("/reliefos/config/display.conf", buffer,
                                 sizeof(buffer), &length) == 0);
    assert(length == sizeof(mock_contents) - 1u);
    assert(memcmp(buffer, mock_contents, length) == 0);
    assert(file_reads == 1);
    assert(efi_fs_read_file_into("/reliefos/config/display.conf", buffer, 2,
                                 &length) == -27);
    assert(length == 0 && file_reads == 1);

    efi_fs_shutdown();
    assert(!efi_fs_ready());
    assert(file_closes == 3);
    calls_after_shutdown = service_calls;
    assert(efi_fs_read_file_into("/reliefos/config/display.conf", buffer,
                                 sizeof(buffer), &length) == -2);
    efi_fs_init((uint64_t)(uintptr_t)&system_table);
    assert(service_calls == calls_after_shutdown);
    assert(efi_boot_memory_map((uint64_t)(uintptr_t)&system_table, memory_map,
                               sizeof(memory_map), &entry_size, &entry_count) == 0);
    assert(entry_size == 8 && entry_count == 2 && memory_map[0] == 0x5a);
    calls_after_final_map = service_calls;
    assert(efi_fs_read_file_into("/reliefos/config/display.conf", buffer,
                                 sizeof(buffer), &length) == -2);
    efi_fs_init((uint64_t)(uintptr_t)&system_table);
    assert(service_calls == calls_after_final_map);
    puts("PASS EFI early filesystem: bounded caller-buffer read, handle cleanup, shutdown sealing");
    return 0;
}
