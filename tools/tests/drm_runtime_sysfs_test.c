/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the native probe's identity checks without opening a host GPU. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int scripted_stat(const char *, struct stat *);
static int scripted_fstat(int, struct stat *);
static ssize_t scripted_readlink(const char *, char *, size_t);
static int scripted_open(const char *, int, ...);
static ssize_t scripted_read(int, void *, size_t);
static int scripted_close(int);
static int scripted_ioctl(int, unsigned long, ...);
#define stat(...) scripted_stat(__VA_ARGS__)
#define fstat(...) scripted_fstat(__VA_ARGS__)
#define readlink scripted_readlink
#define open scripted_open
#define read scripted_read
#define close scripted_close
#define ioctl scripted_ioctl
#define main drm_runtime_entry
#include "drm_runtime_probe.c"
#undef main
#undef stat
#undef fstat
#undef readlink
#undef open
#undef read
#undef close
#undef ioctl

static unsigned vgpu, render_present, bad_identity, query_error, render_opens, render_closes;
static const char *attributes[] = {
    "PCI_ID=15AD:0405\nPCI_SLOT_NAME=0000:00:0f.0\nMODALIAS=pci:v000015ADd00000405\n",
    "226:0\n", "226:128\n"
};

static int scripted_stat(const char *path, struct stat *st)
{
    *st = (struct stat){0};
    if (!strcmp(path, "/sys/class/drm/card0")) { st->st_mode=S_IFDIR|0755;return 0; }
    assert(!strcmp(path,"/dev/dri/renderD128"));
    if (!render_present) { errno=ENOENT;return -1; }
    st->st_mode=(bad_identity==1 ? S_IFREG : S_IFCHR)|0660;
    st->st_rdev=((bad_identity==2 ? 227u : 226u)<<8)|(bad_identity==3 ? 129u : 128u);
    return 0;
}
static int scripted_fstat(int fd, struct stat *st)
{ assert(fd==67);return scripted_stat("/dev/dri/renderD128",st); }
static ssize_t scripted_readlink(const char *path, char *out, size_t length)
{
    const char *target;
    if (!strcmp(path,"/sys/class/drm/card0/device"))target="../..";
    else { assert(!strcmp(path,"/sys/dev/char/226:0/device/subsystem"));target="../../bus/pci"; }
    size_t n=strlen(target);if(n>length)n=length;memcpy(out,target,n);return (ssize_t)n;
}
static int scripted_open(const char *path, int flags, ...)
{
    assert(flags&O_CLOEXEC);
    if (!strcmp(path,"/dev/dri/renderD128")) { ++render_opens;return 67; }
    if (!strcmp(path,"/sys/dev/char/226:0/device/uevent"))return 64;
    if (!strcmp(path,"/sys/dev/char/226:0/device/drm/card0/dev"))return 65;
    assert(!strcmp(path,"/sys/class/drm/renderD128/dev"));return 66;
}
static ssize_t scripted_read(int fd, void *out, size_t length)
{
    assert(fd>=64 && fd<=66);
    const char *s=fd==66 && bad_identity==4 ? "226:129\n" : attributes[fd-64];
    size_t n=strlen(s);if(n>length)n=length;memcpy(out,s,n);return (ssize_t)n;
}
static int scripted_close(int fd)
{ assert(fd>=64 && fd<=67);if(fd==67)++render_closes;return 0; }
static int scripted_ioctl(int fd, unsigned long request, ...)
{
    assert(fd==19 && request==DRM_IOCTL_VMW_GET_PARAM);
    va_list args;va_start(args,request);struct drm_vmw_getparam_arg *p=va_arg(args,void *);va_end(args);
    assert(p->param==DRM_VMW_PARAM_3D);
    if(query_error){errno=EIO;return -1;}p->value=vgpu;return 0;
}
int main(void)
{
    /* A rendering device must accept its correctly numbered render node. */
    vgpu=render_present=1;
    assert(check_sysfs_identity(19)==0);
    assert(render_opens==1 && render_closes==1);
    /* A 2D device must still reject an unexpected render node. */
    vgpu=render_present=0;
    assert(check_sysfs_identity(19)==0);
    render_present=1;assert(check_sysfs_identity(19)!=0);
    vgpu=1;render_present=0;assert(check_sysfs_identity(19)!=0);
    render_present=1;
    for(bad_identity=1;bad_identity<=4;++bad_identity)
        assert(check_sysfs_identity(19)!=0);
    bad_identity=0;query_error=1;assert(check_sysfs_identity(19)!=0);
    query_error=0;vgpu=2;assert(check_sysfs_identity(19)!=0);
    puts("PASS native DRM identity: 2D absence, 3D render identity/open/close, mismatches and failed capability query");
    return 0;
}
