#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/network.h"

static struct task current;
#ifndef NET_INTERFACE_TEST_EXTERNAL_ALLOC
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
#endif
struct task *sched_current_task(void) { return &current; }
static bool nic_present=true, nic_carrier=true;
int e1000_is_ready(void) { return nic_present && nic_carrier; }
void e1000_get_info(struct e1000_info *info)
{ *info=(struct e1000_info){.present=nic_present,.active=nic_present && nic_carrier}; }
void net_ipv6_link_changed(bool up) { (void)up; }
int net_ipv6_link_changed_on(uint32_t index, bool up) { (void)index; (void)up; return 0; }
void netlink_ipv6_changed(void) { }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2,3,4,5,6,7}; return mac; }
uint32_t netdev_next(uint32_t after) { return nic_present && after < 2 ? 2 : 0; }
int netdev_is_ready(uint32_t index) { return index == 2 && e1000_is_ready(); }
const uint8_t *netdev_mac(uint32_t index) { (void)index; return e1000_mac(); }
void netdev_get_info(uint32_t index, struct e1000_info *info)
{ if (index == 2) { e1000_get_info(info); memcpy(info->mac,e1000_mac(),6); } else memset(info,0,sizeof(*info)); }
bool user_range_ok(uint64_t address, uint64_t size) { return address >= 4096 && size <= 4096; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
static int call(unsigned command, struct ifreq *req) { return net_interface_ioctl(command, (uintptr_t)req); }
int main(void)
{
    assert(!net_interface_register(2));
    struct ifreq req = {.ifr_name = "eth0"};
    assert(call(SIOCGIFINDEX, &req) == 0 && req.data.value == 2);
    assert(call(SIOCGIFHWADDR, &req) == 0 && req.data.addr.sa_family == 1 && req.data.addr.sa_data[0] == 2);
    req.data.addr.sa_family = AF_INET;
    net_put_u32((uint8_t *)&req.data.addr + 4, 0x0a00020f);
    assert(call(SIOCSIFADDR, &req) == -LINUX_EPERM);
    current.cap_effective = 1ULL << CAP_NET_ADMIN;
    assert(call(SIOCSIFADDR, &req) == 0 && net_config.local_ip == 0x0a00020f);
    net_put_u32((uint8_t *)&req.data.addr + 4, 0xff00ff00);
    assert(call(SIOCSIFNETMASK, &req) == -LINUX_EINVAL);
    net_put_u32((uint8_t *)&req.data.addr + 4, 0xffffff00);
    assert(call(SIOCSIFNETMASK, &req) == 0 && net_config.subnet_mask == 0xffffff00);
    assert(call(SIOCGIFBRDADDR, &req) == 0);
    assert(net_get_u32((uint8_t *)&req.data.addr + 4) == 0x0a0002ff);
    struct rtentry rt = {.rt_dst.sa_family = AF_INET, .rt_gateway.sa_family = AF_INET,
        .rt_genmask.sa_family = 0, .rt_flags = 3};
    net_put_u32((uint8_t *)&rt.rt_gateway + 4, 0x0a000202);
    assert(net_interface_ioctl(SIOCADDRT, (uintptr_t)&rt) == 0);
    assert(net_config.gateway_ip == 0x0a000202);
    assert(net_interface_ioctl(SIOCADDRT, (uintptr_t)&rt) == -LINUX_EEXIST);
    assert(net_interface_ioctl(SIOCDELRT, (uintptr_t)&rt) == 0);
    assert(net_interface_ioctl(SIOCDELRT, (uintptr_t)&rt) == -LINUX_ESRCH);
    struct ifreq output[2];
    struct ifconf conf = {.ifc_len = sizeof(output), .ifc_buf = (uintptr_t)&output};
    assert(net_interface_ioctl(SIOCGIFCONF, (uintptr_t)&conf) == 0);
    assert(conf.ifc_len == sizeof(output) && !strcmp(output[0].ifr_name, "lo") &&
           !strcmp(output[1].ifr_name, "eth0"));
    assert(net_get_u32((uint8_t *)&output[1].data.addr + 4) == 0x0a00020f);
    req.data.flags = 0;
    assert(call(SIOCSIFFLAGS, &req) == 0 && !net_interface_ready());
    req.data.flags = IFF_UP;
    assert(call(SIOCSIFFLAGS, &req) == 0 && net_interface_ready());
    nic_carrier=false;
    assert(call(SIOCGIFINDEX,&req)==0 && req.data.value==2);
    assert(call(SIOCGIFNAME,&req)==0 && !strcmp(req.ifr_name,"eth0"));
    assert(call(SIOCGIFFLAGS,&req)==0 && (req.data.flags&IFF_UP) &&
           !(req.data.flags&IFF_RUNNING) && !net_interface_ready());
    req.data.flags=0;
    assert(call(SIOCSIFFLAGS,&req)==0);
    req.data.flags=IFF_UP;
    assert(call(SIOCSIFFLAGS,&req)==0);
    assert(call(SIOCGIFMTU,&req)==0 && req.data.value==1500);
    conf.ifc_len=sizeof(output);
    assert(!net_interface_ioctl(SIOCGIFCONF,(uintptr_t)&conf) && conf.ifc_len==sizeof(output));
    nic_present=false;
    net_interface_remove(2);
    assert(call(SIOCGIFINDEX,&req)==-LINUX_ENODEV);
    conf.ifc_len=sizeof(output);
    assert(!net_interface_ioctl(SIOCGIFCONF,(uintptr_t)&conf) && conf.ifc_len==sizeof(struct ifreq));
    nic_present=nic_carrier=true;
    assert(!net_interface_register(2));
    strcpy(req.ifr_name, "absent");
    assert(call(SIOCGIFINDEX, &req) == -LINUX_ENODEV);
    assert(net_interface_ioctl(SIOCGIFINDEX, 0) == -LINUX_EFAULT);
    puts("PASS real kernel interface ioctl permissions, address/mask, route, enumeration, up/down");
}
