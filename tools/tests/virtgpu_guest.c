/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/virtgpu_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** @brief Fail with the real syscall diagnostic rather than continuing a test.
 * @param operation Failed test stage. Does not return.
 */
static void fail(const char *operation)
{
    fprintf(stderr,"VIRTGPU_FAIL %s: errno=%d (%s)\n",operation,errno,strerror(errno));
    exit(1);
}

/** @brief Perform an expected-success ioctl on the actual DRM device.
 * @param fd Open device. @param command Linux ioctl number. @param data Argument.
 */
static void request(int fd,unsigned long command,void *data)
{
    if(ioctl(fd,command,data)<0) {
        fprintf(stderr,"ioctl=%#lx ",command);fail("ioctl");
    }
}

/** @brief Allocate and populate a real mapped 32-bit GEM dumb buffer.
 * @param fd Device. @param width Pixels. @param height Pixels. @param color Base
 * RGB value. @param out Required handle, pitch and size output.
 * @return Owned mapping, released by the caller with munmap.
 */
static uint32_t *buffer(int fd,unsigned width,unsigned height,uint32_t color,
                        struct drm_mode_create_dumb *out)
{
    *out=(struct drm_mode_create_dumb){.width=width,.height=height,.bpp=32};
    request(fd,DRM_IOCTL_MODE_CREATE_DUMB,out);
    struct drm_mode_map_dumb map={.handle=out->handle};
    request(fd,DRM_IOCTL_MODE_MAP_DUMB,&map);
    uint32_t *pixels=mmap(NULL,out->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,map.offset);
    if(pixels==MAP_FAILED)fail("mmap GEM");
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
        pixels[(size_t)y*out->pitch/4+x]=0xff000000u|color|((x*127/width)<<8)|(y*127/height);
    return pixels;
}

/** @brief Exercise canonical DMA-BUF identity, same-device import and mapped pages.
 * @param fd Exporting GEM namespace. @param bo Live allocated resource.
 * @param pixels Its shared CPU mapping. No descriptors or mappings survive return.
 */
static void prime(int fd,const struct drm_mode_create_dumb *bo,uint32_t *pixels)
{
    struct drm_prime_handle export={.handle=bo->handle,.flags=DRM_CLOEXEC|DRM_RDWR};
    request(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&export);
    struct drm_prime_handle again={.handle=bo->handle,.flags=DRM_CLOEXEC|DRM_RDWR};
    request(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&again);
    struct stat a,b;
    if(fstat(export.fd,&a) || fstat(again.fd,&b) || a.st_ino!=b.st_ino || a.st_dev!=b.st_dev ||
       lseek(export.fd,0,SEEK_END)!=(off_t)bo->size || !(fcntl(export.fd,F_GETFD)&FD_CLOEXEC))
        fail("DMA-BUF canonical identity");
    int importer=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(importer<0)fail("open importer");
    struct drm_prime_handle import={.fd=export.fd};
    request(importer,DRM_IOCTL_PRIME_FD_TO_HANDLE,&import);
    struct drm_prime_handle repeat={.fd=export.fd};
    request(importer,DRM_IOCTL_PRIME_FD_TO_HANDLE,&repeat);
    if(repeat.handle!=import.handle){errno=EPROTO;fail("import deduplication");}
    uint32_t *shared=mmap(NULL,bo->size,PROT_READ|PROT_WRITE,MAP_SHARED,export.fd,0);
    if(shared==MAP_FAILED)fail("DMA-BUF mmap");
    uint64_t flags=3;
    request(export.fd,0x40086200,&flags);
    uint32_t old=pixels[0];shared[0]=0xff123456;
    if(pixels[0]!=0xff123456){errno=EIO;fail("shared GEM pages");}
    shared[0]=old;flags=7;request(export.fd,0x40086200,&flags);
    if(close(export.fd) || close(again.fd))fail("DMA-BUF close");
    if(shared[0]!=old || munmap(shared,bo->size))fail("DMA-BUF VMA lifetime");
    struct drm_gem_close close_gem={.handle=import.handle};
    request(importer,DRM_IOCTL_GEM_CLOSE,&close_gem);
    if(close(importer))fail("close importer");
    puts("VIRTGPU_PRIME_PASS canonical=pass import=pass mmap=pass sync=pass lifetime=pass");
}

/** @brief Verify standard Linux sync_file fences when VirGL is negotiated.
 * @param fd DRM authority. Every returned fence is polled, queried and closed.
 */
static void fences(int fd)
{
    int features=0;
    struct drm_virtgpu_getparam param={.param=VIRTGPU_PARAM_3D_FEATURES,.value=(uintptr_t)&features};
    request(fd,DRM_IOCTL_VIRTGPU_GETPARAM,&param);
    if(!features)return;
    uint32_t nop=0;
    struct drm_virtgpu_execbuffer exec={.flags=VIRTGPU_EXECBUF_FENCE_FD_OUT,
        .size=sizeof(nop),.command=(uintptr_t)&nop,.fence_fd=-1};
    request(fd,DRM_IOCTL_VIRTGPU_EXECBUFFER,&exec);
    int first=exec.fence_fd;
    if(first<0 || !(fcntl(first,F_GETFD)&FD_CLOEXEC))fail("sync_file CLOEXEC");
    struct pollfd ready={.fd=first,.events=POLLIN};
    if(poll(&ready,1,3000)!=1 || !(ready.revents&POLLIN))fail("sync_file poll");
    struct sync_file_info info={0};
    request(first,SYNC_IOC_FILE_INFO,&info);
    if(info.status!=1 || info.num_fences!=1){errno=EPROTO;fail("fence info counts");}
    struct sync_fence_info detail;
    info.sync_fence_info=(uintptr_t)&detail;
    request(first,SYNC_IOC_FILE_INFO,&info);
    if(detail.status!=1 || strcmp(detail.driver_name,"virtio_gpu") || !detail.timestamp_ns)
        {errno=EPROTO;fail("fence info status");}
    struct sync_merge_data merge={.name="ReliefNT fence test",.fd2=first,.fence=-1};
    request(first,SYNC_IOC_MERGE,&merge);
    exec.flags=VIRTGPU_EXECBUF_FENCE_FD_IN|VIRTGPU_EXECBUF_FENCE_FD_OUT;
    exec.fence_fd=merge.fence;
    request(fd,DRM_IOCTL_VIRTGPU_EXECBUFFER,&exec);
    if(close(first) || close(merge.fence))fail("close imported fences");
    ready=(struct pollfd){.fd=exec.fence_fd,.events=POLLIN};
    if(poll(&ready,1,3000)!=1 || !(ready.revents&POLLIN))fail("dependent fence");
    if(close(exec.fence_fd))fail("close fence");
    puts("VIRTGPU_FENCE_PASS poll=pass info=pass merge=pass dependency=pass lifetime=pass");
}

/** @brief Check the generated Linux connector attributes against real KMS state.
 * @param mode Preferred mode returned by GETCONNECTOR. Files close before return.
 */
static void connector_sysfs(const struct drm_mode_modeinfo *mode)
{
    static const char base[]="/sys/class/drm/card0-Virtual-1/";
    char path[128],text[2048],expected[64];
    const char *attributes[]={"status","modes","edid","enabled"};
    unsigned edid_bytes=0;
    for(unsigned i=0;i<4;++i) {
        snprintf(path,sizeof(path),"%s%s",base,attributes[i]);
        int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)fail("open connector sysfs");
        ssize_t bytes=read(fd,text,sizeof(text)-1);
        if(bytes<0 || close(fd))fail("read connector sysfs");
        text[bytes]=0;
        if(i==0 && strcmp(text,"connected\n")){errno=EPROTO;fail("sysfs connection");}
        if(i==1) {
            snprintf(expected,sizeof(expected),"%ux%u\n",mode->hdisplay,mode->vdisplay);
            if(!strstr(text,expected)){errno=EPROTO;fail("sysfs preferred mode");}
        }
        if(i==2) {
            edid_bytes=(unsigned)bytes;
            if(bytes && (bytes<128 || (bytes&127))){errno=EPROTO;fail("sysfs EDID length");}
            for(unsigned block=0;block<edid_bytes;block+=128) {
                uint8_t checksum=0;
                for(unsigned j=0;j<128;++j)checksum+=(uint8_t)text[block+j];
                if(checksum){errno=EPROTO;fail("sysfs EDID checksum");}
            }
        }
        if(i==3 && strcmp(text,"enabled\n")){errno=EPROTO;fail("sysfs enabled");}
    }
    printf("VIRTGPU_SYSFS_PASS status=pass modes=pass edid=%u enabled=pass\n",edid_bytes);
}

/** @brief Verify native Linux DRM discovery, 2D scanout, events and cursor queues.
 * @return Zero only after real device ioctls and event records succeed.
 */
int main(int argc,char **argv)
{
    bool pause_for_capture=argc==1;
    if(argc>2 || (argc==2 && strcmp(argv[1],"--no-pause"))) {
        fprintf(stderr,"usage: %s [--no-pause]\n",argv[0]);return 2;
    }
    setvbuf(stdout,NULL,_IONBF,0);
    int fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);
    if(fd<0)fail("open card0");
    char name[64]={0};struct drm_version version={.name=name,.name_len=sizeof(name)-1};
    request(fd,DRM_IOCTL_VERSION,&version);
    if(strcmp(name,"virtio_gpu")){errno=ENODEV;fail("driver identity");}
    printf("VIRTGPU_DRIVER %s %d.%d\n",name,version.version_major,version.version_minor);
    fences(fd);
    struct drm_mode_card_res resources={0};
    request(fd,DRM_IOCTL_MODE_GETRESOURCES,&resources);
    if(!resources.count_crtcs || resources.count_crtcs>16 || !resources.count_connectors ||
       resources.count_connectors>16){errno=EPROTO;fail("KMS counts");}
    uint32_t crtcs[16],connectors[16];
    resources.crtc_id_ptr=(uintptr_t)crtcs;resources.connector_id_ptr=(uintptr_t)connectors;
    resources.count_encoders=0;
    request(fd,DRM_IOCTL_MODE_GETRESOURCES,&resources);
    struct drm_mode_get_connector connector={.connector_id=connectors[0]};
    request(fd,DRM_IOCTL_MODE_GETCONNECTOR,&connector);
    if(connector.connection!=1 || !connector.count_modes || connector.count_modes>64) {
        errno=ENODEV;fail("connected modes");
    }
    struct drm_mode_modeinfo modes[64];
    connector.modes_ptr=(uintptr_t)modes;connector.count_modes=64;
    connector.count_encoders=connector.count_props=0;
    request(fd,DRM_IOCTL_MODE_GETCONNECTOR,&connector);
    struct drm_mode_modeinfo mode=modes[0];
    printf("VIRTGPU_MODE %ux%u outputs=%u\n",mode.hdisplay,mode.vdisplay,resources.count_connectors);
    struct drm_mode_create_dumb bos[3];uint32_t *maps[3];uint32_t fbs[2];
    for(unsigned i=0;i<2;++i) {
        maps[i]=buffer(fd,mode.hdisplay,mode.vdisplay,i ? 0x206000 : 0x602000,&bos[i]);
        struct drm_mode_fb_cmd fb={.width=mode.hdisplay,.height=mode.vdisplay,
            .pitch=bos[i].pitch,.bpp=32,.depth=24,.handle=bos[i].handle};
        request(fd,DRM_IOCTL_MODE_ADDFB,&fb);fbs[i]=fb.fb_id;
    }
    prime(fd,&bos[0],maps[0]);
    struct drm_mode_crtc crtc={.crtc_id=crtcs[0],.fb_id=fbs[0],.mode_valid=1,
        .set_connectors_ptr=(uintptr_t)connectors,.count_connectors=1,.mode=mode};
    request(fd,DRM_IOCTL_MODE_SETCRTC,&crtc);
    connector_sysfs(&mode);
    struct drm_mode_fb_dirty_cmd dirty={.fb_id=fbs[0]};
    request(fd,DRM_IOCTL_MODE_DIRTYFB,&dirty);
    for(unsigned i=0;i<12;++i) {
        struct drm_mode_crtc_page_flip flip={.crtc_id=crtcs[0],.fb_id=fbs[i&1],
            .flags=DRM_MODE_PAGE_FLIP_EVENT,.user_data=0x1122334400000000ULL+i};
        request(fd,DRM_IOCTL_MODE_PAGE_FLIP,&flip);
        struct pollfd pollfd={.fd=fd,.events=POLLIN};
        if(poll(&pollfd,1,2000)!=1 || !(pollfd.revents&POLLIN))fail("flip poll");
        struct drm_event_vblank event;
        ssize_t bytes=read(fd,&event,sizeof(event));
        if(bytes!=sizeof(event) || event.base.type!=DRM_EVENT_FLIP_COMPLETE ||
           event.base.length!=sizeof(event) || event.user_data!=flip.user_data ||
           event.crtc_id!=crtcs[0]){errno=EPROTO;fail("flip event");}
    }
    maps[2]=buffer(fd,64,64,0xffffff,&bos[2]);
    struct drm_mode_cursor2 cursor={.flags=DRM_MODE_CURSOR_BO|DRM_MODE_CURSOR_MOVE,
        .crtc_id=crtcs[0],.width=64,.height=64,.handle=bos[2].handle,.x=100,.y=100};
    request(fd,DRM_IOCTL_MODE_CURSOR2,&cursor);
    cursor.flags=DRM_MODE_CURSOR_MOVE;cursor.x=320;cursor.y=240;
    request(fd,DRM_IOCTL_MODE_CURSOR2,&cursor);
    struct drm_mode_map_dumb bad={.handle=0xffffffffu};
    errno=0;if(ioctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&bad)!=-1 || errno!=ENOENT)fail("invalid handle rejection");
    if(pause_for_capture) {
        printf("VIRTGPU_SCREENSHOT_READY\n");
        char resume;
        if(read(STDIN_FILENO,&resume,1)!=1)fail("screenshot handshake");
    }
    cursor.flags=DRM_MODE_CURSOR_BO;cursor.handle=0;
    request(fd,DRM_IOCTL_MODE_CURSOR2,&cursor);
    crtc.mode_valid=0;request(fd,DRM_IOCTL_MODE_SETCRTC,&crtc);
    for(unsigned i=0;i<2;++i)request(fd,DRM_IOCTL_MODE_RMFB,&fbs[i]);
    for(unsigned i=0;i<3;++i) {
        if(munmap(maps[i],bos[i].size))fail("munmap");
        struct drm_mode_destroy_dumb destroy={.handle=bos[i].handle};
        request(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy);
    }
    if(close(fd))fail("close");
    puts("VIRTGPU_KMS_PASS modeset=pass dirty=pass flip=12 cursor=pass teardown=pass");
    return 0;
}
