/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
__attribute__((noreturn)) void address_space_map_user_page(void){__builtin_trap();}
__attribute__((noreturn)) void kernel_free(void){__builtin_trap();}
__attribute__((noreturn)) void kernel_malloc(void){__builtin_trap();}
__attribute__((noreturn)) void mm_alloc_page(void){__builtin_trap();}
__attribute__((noreturn)) void mm_free_page(void){__builtin_trap();}
__attribute__((noreturn)) void mm_phys_to_virt(void){__builtin_trap();}
__attribute__((noreturn)) void mm_retain_page(void){__builtin_trap();}
__attribute__((noreturn)) void sched_current_task(void){__builtin_trap();}
__attribute__((noreturn)) void sched_task_vma_at(void){__builtin_trap();}
__attribute__((noreturn)) void sched_task_vma_capacity(void){__builtin_trap();}
__attribute__((noreturn)) void syscall_mm_map_sysv_shm(void){__builtin_trap();}
__attribute__((noreturn)) void syscall_mm_munmap(void){__builtin_trap();}
