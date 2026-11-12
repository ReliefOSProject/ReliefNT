/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: net ioctl user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <stdlib.h>
#define NET_INTERFACE_TEST_EXTERNAL_ALLOC
#define main inherited_interface_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "net_interface_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefos/net_control.h>

static uint64_t blocked;
static bool block_output;
static struct task_file net_file={.used=1,.flags=TASK_FILE_FLAG_SOCKET_INET};
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool output)
{ assert(task==&current && length<=32768);return address==blocked && output==block_output ? SWAP_STORAGE_CONTENDED:0; }
void storage_drain_task_io(uint32_t pid) { assert(pid==current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode;assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode;assert(0);return 0; }
void task_process_vm_scope_release(void *state) { (void)state;assert(0); }

static int scoped(unsigned request,uint64_t address)
{
    struct trap_frame f={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=request,.rdx=address};
    task_lookup_scope_begin(&current,&f);
    return task_net_control(&net_file,request,address);
}
int main(void)
{
    assert(net_interface_register(2) == 0);
    current=(struct task){.kind=TASK_KIND_USER,.pid=45,.syscall_file=&net_file,
                          .syscall_fd=3,.syscall_file_number=LINUX_SYS_IOCTL,
                          .cap_effective=1ULL<<CAP_NET_ADMIN};
    struct ifreq req={.ifr_name="eth0"};
    blocked=(uintptr_t)&req;block_output=true;
    assert(scoped(SIOCGIFINDEX,(uintptr_t)&req)==KERNEL_SYSCALL_SWAP_CONTENDED);
    strcpy(req.ifr_name,"nope");blocked=0;
    assert(!scoped(SIOCGIFINDEX,(uintptr_t)&req) && req.data.value==2);
    task_lookup_scope_cancel(&current);
    struct reliefos_net_control ctrl={.version=RELIEFOS_NET_CONTROL_VERSION,
                                      .operation=RELIEFOS_NET_CONTROL_CONFIG};
    blocked=(uintptr_t)&ctrl;block_output=true;
    assert(scoped(RELIEFOS_NET_CONTROL_IOCTL,(uintptr_t)&ctrl)==KERNEL_SYSCALL_SWAP_CONTENDED);
    ctrl.version=999;blocked=0;
    assert(!scoped(RELIEFOS_NET_CONTROL_IOCTL,(uintptr_t)&ctrl) && ctrl.version==RELIEFOS_NET_CONTROL_VERSION);
    task_lookup_scope_cancel(&current);
    struct ifreq output={0};struct ifconf conf={.ifc_len=sizeof(output),.ifc_buf=(uintptr_t)&output};
    net_config.local_ip=0x0a00020f;blocked=(uintptr_t)&output;block_output=true;
    assert(scoped(SIOCGIFCONF,(uintptr_t)&conf)==KERNEL_SYSCALL_SWAP_CONTENDED);
    conf.ifc_buf=0;blocked=0;
    assert(!scoped(SIOCGIFCONF,(uintptr_t)&conf) && !strcmp(output.ifr_name,"lo"));
    task_lookup_scope_cancel(&current);
    char route_name[]="eth0";
    struct rtentry route={.rt_dst.sa_family=AF_INET,.rt_gateway.sa_family=AF_INET,
                          .rt_flags=3,.rt_dev=(uintptr_t)route_name};
    net_config.subnet_mask=0xffffff00;net_config.gateway_ip=0;net_interface_up=true;
    net_put_u32((uint8_t *)&route.rt_gateway+4,0x0a000202);
    blocked=(uintptr_t)(route_name+2);block_output=false;
    assert(scoped(SIOCADDRT,(uintptr_t)&route)==KERNEL_SYSCALL_SWAP_CONTENDED);
    route_name[0]='X';net_put_u32((uint8_t *)&route.rt_gateway+4,0x0a000203);blocked=0;
    assert(!scoped(SIOCADDRT,(uintptr_t)&route) && net_config.gateway_ip==0x0a000202);
    task_lookup_scope_cancel(&current);
    assert(scoped(0x89fe,1)==-LINUX_ENOTTY);task_lookup_scope_cancel(&current);
    puts("PASS INET ioctls: fixed and nested output waits, immutable inputs, ignored unknown operand");
    return 0;
}
