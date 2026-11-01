/* SPDX-License-Identifier: Apache-2.0 */
/* These alternative dispatcher branches are unreachable in this fixture. */
#define UNREACHED(name) __attribute__((noreturn)) void name(void) { __builtin_trap(); }
UNREACHED(storage_disk_block_read)
UNREACHED(storage_disk_block_write)
UNREACHED(storage_sync_disk)
UNREACHED(kernel_wait_queue_remove)
UNREACHED(task_signalfd_readv)
UNREACHED(task_socket_vector)
UNREACHED(task_socket_vector_user)
UNREACHED(task_pty_for_io)
UNREACHED(syscall_io_read)
UNREACHED(syscall_io_write)
UNREACHED(syscall_getdents_object)
UNREACHED(syscall_copy_file_range)
UNREACHED(syscall_sendfile)
UNREACHED(task_device_is)
UNREACHED(syscall_seek_object)
UNREACHED(task_lookup_user_bytes)
