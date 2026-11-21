/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define VMW_GET_PARAM DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM, struct drm_vmw_getparam_arg)
#define VMW_ALLOC_BO DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO, union drm_vmw_alloc_bo_arg)
#define VMW_HANDLE_CLOSE DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_HANDLE_CLOSE, struct drm_vmw_handle_close_arg)
#define VMW_UPDATE_LAYOUT DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UPDATE_LAYOUT, struct drm_vmw_update_layout_arg)
#define VMW_MSG DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_MSG, struct drm_vmw_msg_arg)
#define VMW_MKSSTAT_RESET DRM_IO(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_RESET)
#define VMW_MKSSTAT_ADD DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_ADD, struct drm_vmw_mksstat_add_arg)
#define VMW_MKSSTAT_REMOVE DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_REMOVE, struct drm_vmw_mksstat_remove_arg)

static void verify_host_gate(int fd)
{
    const char text[] = "log ReliefNT SVGA gate probe";
    struct drm_vmw_msg_arg msg = {.send = (uintptr_t)text, .send_only = 1};
    struct drm_vmw_mksstat_add_arg add = {0};
    struct drm_vmw_mksstat_remove_arg remove = {0};

    assert(ioctl(fd, VMW_MSG, &msg) == -1 && errno == ENODEV);
    assert(ioctl(fd, VMW_MKSSTAT_RESET, 0) == -1 && errno == ENODEV);
    assert(ioctl(fd, VMW_MKSSTAT_ADD, &add) == -1 && errno == ENODEV);
    assert(ioctl(fd, VMW_MKSSTAT_REMOVE, &remove) == -1 && errno == ENODEV);
    puts("SVGA_HOST_GATE_PASS msg=pass mksstat=pass");
}

static void verify_layout_event(int monitor)
{
    char payload[512];
    char control[CMSG_SPACE(sizeof(struct ucred))];
    struct sockaddr_nl source = {0};
    struct iovec iov = {.iov_base = payload, .iov_len = sizeof(payload)};
    struct msghdr msg = {
        .msg_name = &source, .msg_namelen = sizeof(source), .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control),
    };
    struct pollfd waiter = {.fd = monitor, .events = POLLIN};
    assert(poll(&waiter, 1, 1000) == 1 && waiter.revents == POLLIN);
    ssize_t bytes = recvmsg(monitor, &msg, 0);
    assert(bytes > 0 && bytes <= (ssize_t)sizeof(payload) && !msg.msg_flags && !payload[bytes - 1]);
    assert(source.nl_family == AF_NETLINK && source.nl_pid == 0 && source.nl_groups == 1);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
           cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred)));
    struct ucred creds;
    memcpy(&creds, CMSG_DATA(cmsg), sizeof(creds));
    assert(creds.pid == 0 && creds.uid == 0 && creds.gid == 0);
    assert(!strncmp(payload, "change@/devices/", sizeof("change@/devices/") - 1));
    bool hotplug = false, drm = false, path = false;
    for (const char *s = payload; s < payload + bytes; s += strlen(s) + 1) {
        hotplug |= !strcmp(s, "HOTPLUG=1");
        drm |= !strcmp(s, "SUBSYSTEM=drm");
        if (!strncmp(s, "DEVPATH=", sizeof("DEVPATH=") - 1)) {
            char name[512];
            struct stat st;
            assert(!strcmp(s + 8, payload + 7));
            int length = snprintf(name, sizeof(name), "/sys%s", s + 8);
            assert(length > 0 && length < (int)sizeof(name));
            path = !stat(name, &st) && S_ISDIR(st.st_mode);
        }
    }
    assert(hotplug && drm && path);
}

static void call(int fd, unsigned long request, void *arg)
{
    if (ioctl(fd, request, arg) == -1) {
        fprintf(stderr, "SVGA_FAIL ioctl %#lx: %s\n", request, strerror(errno));
        exit(1);
    }
}

static uint32_t property_id(int fd, uint32_t object, uint32_t type, const char *name,
                            uint64_t *value)
{
    uint32_t ids[24];
    uint64_t values[24];
    struct drm_mode_obj_get_properties props = {
        .obj_id = object, .obj_type = type, .count_props = 24,
        .props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)values,
    };
    call(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props);
    assert(props.count_props <= 24);
    for (unsigned i = 0; i < props.count_props; ++i) {
        struct drm_mode_get_property prop = {.prop_id = ids[i]};
        call(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
        if (!strcmp(prop.name, name)) {
            if (value)
                *value = values[i];
            return ids[i];
        }
    }
    fprintf(stderr, "SVGA_FAIL property %s\n", name);
    exit(1);
}

static void verify_atomic(int fd, const struct drm_mode_crtc *crtc, uint32_t connector_id)
{
    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
    call(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    uint64_t old_blob;
    uint32_t mode_id = property_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &old_blob);
    assert(old_blob);
    struct drm_mode_create_blob create = {.data = (uintptr_t)&crtc->mode, .length = sizeof(crtc->mode)};
    call(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &create);
    uint32_t objects[] = {crtc->crtc_id, connector_id};
    uint32_t counts[] = {2, 1};
    uint32_t properties[] = {
        mode_id, property_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL),
        property_id(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL),
    };
    uint64_t values[] = {create.blob_id, 0, crtc->crtc_id};
    struct drm_mode_atomic atomic = {
        .flags = DRM_MODE_ATOMIC_TEST_ONLY, .count_objs = 2, .user_data = 0x5432,
        .objs_ptr = (uintptr_t)objects, .count_props_ptr = (uintptr_t)counts,
        .props_ptr = (uintptr_t)properties, .prop_values_ptr = (uintptr_t)values,
    };
    assert(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == -1 && errno == EINVAL);
    atomic.flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    values[2] = UINT32_MAX;
    assert(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == -1 && errno == EINVAL);
    values[2] = crtc->crtc_id;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    uint64_t current;
    property_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &current);
    assert(current == old_blob);
    atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    property_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &current);
    assert(!current);

    struct drm_mode_destroy_blob destroy = {.blob_id = create.blob_id};
    call(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy);
    struct drm_mode_modeinfo read_mode = {0};
    struct drm_mode_get_blob get = {
        .blob_id = create.blob_id, .length = sizeof(read_mode), .data = (uintptr_t)&read_mode,
    };
    call(fd, DRM_IOCTL_MODE_GETPROPBLOB, &get);
    assert(read_mode.hdisplay == crtc->mode.hdisplay && read_mode.vdisplay == crtc->mode.vdisplay);
    atomic.count_objs = 1;
    counts[0] = 1;
    properties[0] = properties[1];
    values[0] = 1;
    atomic.flags |= DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    struct pollfd waiter = {.fd = fd, .events = POLLIN};
    assert(poll(&waiter, 1, 1000) == 1 && waiter.revents == POLLIN);
    struct drm_event_vblank event;
    assert(read(fd, &event, sizeof(event)) == sizeof(event));
    assert(event.base.type == DRM_EVENT_FLIP_COMPLETE && event.user_data == atomic.user_data &&
           event.crtc_id == crtc->crtc_id);
    property_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &current);
    assert(current == 1);
    puts("SVGA_ATOMIC_PASS test_only=pass mode_blob=retained inactive=pass reactivated=pass nonblock_event=pass");
}

static void verify_atomic_event_targets(int fd, const uint32_t *crtcs)
{
    uint32_t counts[] = {1, 1};
    uint32_t props[] = {
        property_id(fd, crtcs[0], DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL),
        property_id(fd, crtcs[1], DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL),
    };
    uint64_t values[] = {0, 0};
    struct drm_mode_atomic atomic = {
        .flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY,
        .count_objs = 2, .objs_ptr = (uintptr_t)crtcs, .count_props_ptr = (uintptr_t)counts,
        .props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)values,
    };
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK |
                   DRM_MODE_PAGE_FLIP_EVENT;
    assert(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == -1 && errno == EINVAL);

    uint64_t current;
    property_id(fd, crtcs[0], DRM_MODE_OBJECT_CRTC, "ACTIVE", &current);
    assert(current == 1);
    struct pollfd waiter = {.fd = fd, .events = POLLIN};
    assert(!poll(&waiter, 1, 0));
    puts("SVGA_ATOMIC_EVENT_TARGETS_PASS inactive_crtc=EINVAL first_crtc_unchanged=pass");
}

/* Give each active output a different BO, then swap both framebuffer bindings
 * in a single nonblocking atomic transaction. One event per CRTC is required. */
static void verify_dual_output(int fd, const struct drm_mode_crtc *first,
                               const uint32_t *crtcs, const uint32_t *connectors)
{
    uint32_t width = first->mode.hdisplay, height = first->mode.vdisplay;
    struct drm_vmw_rect outputs[] = {{.w = width, .h = height}, {.x = width, .w = width, .h = height}};
    struct drm_vmw_update_layout_arg layout = {.num_outputs = 2, .rects = (uintptr_t)outputs};
    call(fd, VMW_UPDATE_LAYOUT, &layout);

    struct drm_mode_create_dumb dumb = {.width = width, .height = height, .bpp = 32};
    call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    struct drm_mode_map_dumb map = {.handle = dumb.handle};
    call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    uint32_t *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    assert(pixels != MAP_FAILED);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            pixels[y * (dumb.pitch / 4) + x] = 0xff904030;
    struct drm_mode_fb_cmd fb = {.width = width, .height = height, .pitch = dumb.pitch,
                                 .bpp = 32, .depth = 24, .handle = dumb.handle};
    call(fd, DRM_IOCTL_MODE_ADDFB, &fb);
    struct drm_mode_crtc second = *first;
    second.crtc_id = crtcs[1];
    second.fb_id = fb.fb_id;
    second.set_connectors_ptr = (uintptr_t)&connectors[1];
    call(fd, DRM_IOCTL_MODE_SETCRTC, &second);

    uint32_t planes[16], objects[2] = {0};
    struct drm_mode_get_plane_res resources = {.count_planes = 16, .plane_id_ptr = (uintptr_t)planes};
    call(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources);
    assert(resources.count_planes <= 16);
    for (unsigned i = 0; i < resources.count_planes && !(objects[0] && objects[1]); ++i) {
        struct drm_mode_get_plane plane = {.plane_id = planes[i]};
        call(fd, DRM_IOCTL_MODE_GETPLANE, &plane);
        if (plane.crtc_id == crtcs[0] && plane.fb_id == first->fb_id)
            objects[0] = plane.plane_id;
        else if (plane.crtc_id == crtcs[1] && plane.fb_id == second.fb_id)
            objects[1] = plane.plane_id;
    }
    assert(objects[0] && objects[1]);
    uint32_t counts[] = {1, 1}, properties[] = {
        property_id(fd, objects[0], DRM_MODE_OBJECT_PLANE, "FB_ID", NULL),
        property_id(fd, objects[1], DRM_MODE_OBJECT_PLANE, "FB_ID", NULL),
    };
    uint64_t values[] = {second.fb_id, first->fb_id};
    struct drm_mode_atomic atomic = {
        .flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, .count_objs = 2,
        .objs_ptr = (uintptr_t)objects, .count_props_ptr = (uintptr_t)counts,
        .props_ptr = (uintptr_t)properties, .prop_values_ptr = (uintptr_t)values, .user_data = 0x4321,
    };
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        struct pollfd waiter = {.fd = fd, .events = POLLIN};
        assert(poll(&waiter, 1, 1000) == 1 && (waiter.revents & POLLIN));
        struct drm_event_vblank event;
        assert(read(fd, &event, sizeof(event)) == sizeof(event));
        assert(event.base.type == DRM_EVENT_FLIP_COMPLETE && event.base.length == sizeof(event) &&
               event.user_data == atomic.user_data);
        assert(event.crtc_id == crtcs[0] || event.crtc_id == crtcs[1]);
        unsigned bit = 1u << (event.crtc_id == crtcs[1]);
        assert(!(seen & bit));
        seen |= bit;
    }
    for (unsigned i = 0; i < 2; ++i) {
        struct drm_mode_crtc current = {.crtc_id = crtcs[i]};
        call(fd, DRM_IOCTL_MODE_GETCRTC, &current);
        assert(current.mode_valid && current.fb_id == values[i]);
    }

    second.mode_valid = second.count_connectors = 0;
    call(fd, DRM_IOCTL_MODE_SETCRTC, &second);
    struct drm_mode_crtc restore = *first;
    call(fd, DRM_IOCTL_MODE_SETCRTC, &restore);
    layout.num_outputs = 0;
    call(fd, VMW_UPDATE_LAYOUT, &layout);
    call(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
    assert(!munmap(pixels, dumb.size));
    call(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb.handle);
    puts("SVGA_KMS_MULTI_PASS outputs=2 atomic_swap=pass flip_events=2");
}

static void verify_vblank(int fd, uint32_t crtc_id)
{
    struct drm_crtc_get_sequence before = {.crtc_id = crtc_id};
    call(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &before);
    assert(before.active);
    union drm_wait_vblank wait = {.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 2}};
    call(fd, DRM_IOCTL_WAIT_VBLANK, &wait);
    assert((uint32_t)(wait.reply.sequence - (uint32_t)before.sequence) >= 2);
    struct drm_crtc_queue_sequence queue = {
        .crtc_id = crtc_id, .flags = DRM_CRTC_SEQUENCE_RELATIVE, .sequence = 1, .user_data = 0x79ab,
    };
    call(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &queue);
    struct pollfd waiter = {.fd = fd, .events = POLLIN};
    assert(poll(&waiter, 1, 1000) == 1 && waiter.revents == POLLIN);
    struct drm_event_crtc_sequence event;
    assert(read(fd, &event, sizeof(event)) == sizeof(event));
    assert(event.base.type == DRM_EVENT_CRTC_SEQUENCE && event.user_data == queue.user_data &&
           event.time_ns > before.sequence_ns && event.sequence >= queue.sequence);
    puts("SVGA_VBLANK_PASS relative_wait=pass queued_sequence=pass");
}

static void verify_damage(int fd, uint32_t *pixels, uint32_t pitch)
{
    uint32_t planes[16];
    const uint32_t capacity = sizeof(planes) / sizeof(planes[0]);
    struct drm_mode_get_plane_res resources = {
        .count_planes = capacity, .plane_id_ptr = (uintptr_t)planes,
    };
    call(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources);
    assert(resources.count_planes >= 1 && resources.count_planes <= capacity);
    uint32_t plane = planes[0], count = 1;
    uint32_t property = property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_DAMAGE_CLIPS", NULL);
    struct drm_mode_rect clip = {32, 32, 48, 48};
    struct drm_mode_create_blob create = {.data = (uintptr_t)&clip, .length = sizeof(clip) - 1};
    call(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &create);
    uint64_t value = create.blob_id;
    struct drm_mode_atomic atomic = {
        .flags = DRM_MODE_ATOMIC_TEST_ONLY, .count_objs = 1,
        .objs_ptr = (uintptr_t)&plane, .count_props_ptr = (uintptr_t)&count,
        .props_ptr = (uintptr_t)&property, .prop_values_ptr = (uintptr_t)&value,
    };
    assert(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == -1 && errno == EINVAL);
    struct drm_mode_destroy_blob destroy = {.blob_id = create.blob_id};
    call(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy);

    create.length = sizeof(clip);
    call(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &create);
    value = create.blob_id;
    for (unsigned y = 32; y < 48; ++y)
        for (unsigned x = 32; x < 48; ++x)
            pixels[y * (pitch / 4) + x] = 0xffe09020;
    pixels[30 * (pitch / 4) + 30] = 0xffffffff;
    atomic.flags = 0;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    destroy.blob_id = create.blob_id;
    call(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy);
    property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_DAMAGE_CLIPS", &value);
    assert(value == destroy.blob_id);
    puts("SVGA_DAMAGE_PASS malformed_blob=EINVAL clip=committed blob=retained");
}

static void verify_cursor_plane(int fd, uint32_t crtc_id)
{
    uint32_t planes[16];
    struct drm_mode_get_plane_res resources = {
        .count_planes = 16, .plane_id_ptr = (uintptr_t)planes,
    };
    call(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources);
    uint32_t primary_count = resources.count_planes;
    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, .value = 1};
    call(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    resources.count_planes = 16;
    call(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources);
    assert(primary_count && resources.count_planes == 2 * primary_count && resources.count_planes <= 16);
    uint32_t plane = planes[primary_count];
    uint64_t type;
    property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "type", &type);
    assert(type == 2);

    struct drm_mode_create_dumb dumb = {.width = 32, .height = 32, .bpp = 32};
    call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    struct drm_mode_fb_cmd2 fb = {
        .width = 32, .height = 32, .pixel_format = 0x34325241u, /* AR24 */
        .handles = {dumb.handle}, .pitches = {dumb.pitch},
    };
    call(fd, DRM_IOCTL_MODE_ADDFB2, &fb);
    const char *names[] = {"CRTC_ID", "FB_ID", "SRC_W", "SRC_H", "CRTC_W", "CRTC_H",
                           "CRTC_X", "CRTC_Y", "HOTSPOT_X", "HOTSPOT_Y"};
    uint32_t count = sizeof(names) / sizeof(names[0]), props[10];
    uint64_t values[] = {crtc_id, fb.fb_id, 32u << 16, 32u << 16, 32, 32, 50, 60, 3, 4};
    for (unsigned i = 0; i < count; ++i)
        props[i] = property_id(fd, plane, DRM_MODE_OBJECT_PLANE, names[i], NULL);
    struct drm_mode_atomic atomic = {
        .flags = DRM_MODE_ATOMIC_TEST_ONLY, .count_objs = 1,
        .objs_ptr = (uintptr_t)&plane, .count_props_ptr = (uintptr_t)&count,
        .props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)values,
    };
    values[5] = 31;
    assert(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == -1 && errno == EINVAL);
    values[5] = 32;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    atomic.flags = 0;
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    struct drm_mode_get_plane readback = {.plane_id = plane};
    call(fd, DRM_IOCTL_MODE_GETPLANE, &readback);
    assert(readback.crtc_id == crtc_id && readback.fb_id == fb.fb_id);
    count = 1;
    values[6] = (uint64_t)(int64_t)-4;
    atomic.props_ptr = (uintptr_t)&props[6];
    atomic.prop_values_ptr = (uintptr_t)&values[6];
    call(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    uint64_t x;
    property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", &x);
    assert((int64_t)x == -4);
    call(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
    readback.count_format_types = 0;
    call(fd, DRM_IOCTL_MODE_GETPLANE, &readback);
    assert(!readback.fb_id && !readback.crtc_id);
    call(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb.handle);
    puts("SVGA_CURSOR_PLANE_PASS hotspot_cap=pass no_scaling=pass move=pass rmfb=pass");
}

static void verify_bo_mapping(int fd)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    union drm_vmw_alloc_bo_arg alloc = {.req.size = 2 * page};
    call(fd, VMW_ALLOC_BO, &alloc);
    assert(alloc.rep.handle && alloc.rep.cur_gmr_id == alloc.rep.handle);
    volatile uint32_t *shared = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, alloc.rep.map_handle);
    assert(shared != MAP_FAILED);
    struct drm_vmw_handle_close_arg close_bo = {.handle = alloc.rep.handle};
    call(fd, VMW_HANDLE_CLOSE, &close_bo);
    shared[0] = 0x12345678;
    size_t second = page / sizeof(*shared);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(shared[0] == 0x12345678);
        shared[second] = 0xaabbccdd;
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(mprotect((void *)shared, page, PROT_READ) == 0);
    assert(shared[0] == 0x12345678 && shared[second] == 0xaabbccdd);
    /* The split leaves a movable VMA with a nonzero BO offset. */
    void *destination = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(destination != MAP_FAILED);
    volatile uint32_t *moved = mremap((void *)(shared + second), page, page,
                                      MREMAP_MAYMOVE | MREMAP_FIXED, destination);
    assert(moved == destination);
    assert(shared[0] == 0x12345678 && moved[0] == 0xaabbccdd);
    moved[0] = 0x55aa55aa;
    assert(mprotect((void *)shared, page, PROT_READ | PROT_WRITE) == 0);
    shared[0] = 0x87654321;
    assert(shared[0] == 0x87654321 && moved[0] == 0x55aa55aa);
    assert(munmap((void *)moved, page) == 0 && munmap((void *)shared, page) == 0);
    puts("SVGA_MAP_PASS fork=mapped mprotect=pass mremap=pass handle_close=pass");
}

int main(int argc, char **argv)
{
    bool pause_for_capture = argc == 1;
    bool expect_3d = argc == 2 && !strcmp(argv[1], "--vmware");
    if (argc > 2 || (argc == 2 && !expect_3d && strcmp(argv[1], "--no-pause")))
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror("SVGA_FAIL open card0");
        return 1;
    }
    char name[32] = {0};
    struct drm_version version = {.name = name, .name_len = sizeof(name) - 1};
    call(fd, DRM_IOCTL_VERSION, &version);
    assert(!strcmp(name, "vmwgfx"));
    struct drm_vmw_getparam_arg param = {.param = DRM_VMW_PARAM_DEVICE_ID};
    call(fd, VMW_GET_PARAM, &param);
    assert(param.value == 0x0405);
    param.param = DRM_VMW_PARAM_3D;
    call(fd, VMW_GET_PARAM, &param);
    assert((param.value != 0) == expect_3d);
    if (expect_3d) {
        assert(access("/dev/dri/renderD128", F_OK) == 0);
    } else {
        assert(access("/dev/dri/renderD128", F_OK) == -1 && errno == ENOENT);
        verify_host_gate(fd);
    }

    uint32_t connectors[32], crtcs[32];
    struct drm_mode_card_res resources = {
        .count_crtcs = sizeof(crtcs) / sizeof(crtcs[0]),
        .count_connectors = sizeof(connectors) / sizeof(connectors[0]),
        .crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)connectors,
    };
    call(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
    assert(resources.count_crtcs && resources.count_crtcs <= sizeof(crtcs) / sizeof(crtcs[0]));
    assert(resources.count_connectors && resources.count_connectors <= sizeof(connectors) / sizeof(connectors[0]));
    if (!expect_3d)
        assert(resources.count_crtcs == 1 && resources.count_connectors == 1);
    uint32_t connector_id = connectors[0], crtc_id = crtcs[0];
    printf("SVGA_KMS_RESOURCES crtcs=%u connectors=%u\n", resources.count_crtcs, resources.count_connectors);
    struct drm_mode_modeinfo modes[16];
    struct drm_mode_get_connector connector = {
        .connector_id = connector_id, .count_modes = 16, .modes_ptr = (uintptr_t)modes,
    };
    call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
    assert(connector.connection == 1 && connector.count_modes <= 16);
    struct drm_mode_modeinfo mode = {0};
    for (unsigned i = 0; i < connector.count_modes; ++i)
        if (modes[i].hdisplay == 800 && modes[i].vdisplay == 600)
            mode = modes[i];
    assert(mode.hdisplay == 800);

    int monitor = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    assert(monitor >= 0);
    int passcred = 1;
    assert(!setsockopt(monitor, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)));
    struct sockaddr_nl subscription = {.nl_family = AF_NETLINK, .nl_groups = 1};
    assert(!bind(monitor, (struct sockaddr *)&subscription, sizeof(subscription)));
    struct drm_vmw_rect output = {.w = 960, .h = 540};
    struct drm_vmw_update_layout_arg layout = {.num_outputs = 1, .rects = (uintptr_t)&output};
    call(fd, VMW_UPDATE_LAYOUT, &layout);
    verify_layout_event(monitor);
    output.x = INT32_MAX;
    assert(ioctl(fd, VMW_UPDATE_LAYOUT, &layout) == -1 && errno == ERANGE);
    struct pollfd no_event = {.fd = monitor, .events = POLLIN};
    assert(!poll(&no_event, 1, 0));
    connector.count_modes = 16;
    connector.count_encoders = connector.count_props = 0;
    call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
    assert(modes[0].hdisplay == 960 && modes[0].vdisplay == 540 &&
           (modes[0].type & DRM_MODE_TYPE_PREFERRED));
    layout.num_outputs = 0;
    call(fd, VMW_UPDATE_LAYOUT, &layout);
    verify_layout_event(monitor);
    close(monitor);
    puts("SVGA_LAYOUT_EVENT_PASS kernel_credentials=pass invalid_no_event=pass reset=pass");
    connector.count_modes = 16;
    connector.count_encoders = connector.count_props = 0;
    call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
    assert(modes[0].hdisplay == 1280 && modes[0].vdisplay == 800);

    struct drm_mode_create_dumb dumb = {.width = 800, .height = 600, .bpp = 32};
    call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    struct drm_mode_map_dumb map = {.handle = dumb.handle};
    call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    uint32_t *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    assert(pixels != MAP_FAILED);
    for (unsigned y = 0; y < dumb.height; ++y)
        for (unsigned x = 0; x < dumb.width; ++x)
            pixels[y * (dumb.pitch / 4) + x] = 0xff2060a0;

    struct drm_mode_fb_cmd fb = {
        .width = dumb.width, .height = dumb.height, .pitch = dumb.pitch,
        .bpp = 32, .depth = 24, .handle = dumb.handle,
    };
    call(fd, DRM_IOCTL_MODE_ADDFB, &fb);
    struct drm_mode_crtc crtc = {
        .crtc_id = crtc_id, .fb_id = fb.fb_id, .mode_valid = 1, .mode = mode,
        .count_connectors = 1, .set_connectors_ptr = (uintptr_t)&connector_id,
    };
    call(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
    struct drm_mode_crtc readback = {.crtc_id = crtc_id};
    call(fd, DRM_IOCTL_MODE_GETCRTC, &readback);
    assert(readback.mode_valid && readback.fb_id == fb.fb_id && readback.mode.hdisplay == 800);

    verify_atomic(fd, &crtc, connector_id);
    if (resources.count_crtcs > 1 && resources.count_connectors > 1) {
        verify_atomic_event_targets(fd, crtcs);
        verify_dual_output(fd, &crtc, crtcs, connectors);
    }
    verify_vblank(fd, crtc_id);
    verify_cursor_plane(fd, crtc_id);

    struct drm_get_cap cursor_width = {.capability = DRM_CAP_CURSOR_WIDTH};
    call(fd, DRM_IOCTL_GET_CAP, &cursor_width);
    assert(cursor_width.value >= 32);
    struct drm_mode_cursor2 cursor = {
        .crtc_id = crtc_id, .flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
        .handle = dumb.handle, .width = 32, .height = 32, .x = 100, .y = 100,
        .hot_x = 3, .hot_y = 4,
    };
    call(fd, DRM_IOCTL_MODE_CURSOR2, &cursor);
    cursor.width = UINT32_MAX;
    assert(ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &cursor) == -1 && errno == EINVAL);
    cursor.flags = DRM_MODE_CURSOR_MOVE;
    cursor.x = 120;
    call(fd, DRM_IOCTL_MODE_CURSOR2, &cursor);
    cursor.flags = DRM_MODE_CURSOR_BO;
    cursor.handle = 0;
    call(fd, DRM_IOCTL_MODE_CURSOR2, &cursor);

    struct drm_clip_rect clip = {.x1 = 8, .y1 = 8, .x2 = 24, .y2 = 24};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = fb.fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};
    clip.x2 = 801;
    assert(ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) == -1 && errno == EINVAL);
    clip.x2 = 24;
    for (unsigned y = clip.y1; y < clip.y2; ++y)
        for (unsigned x = clip.x1; x < clip.x2; ++x)
            pixels[y * (dumb.pitch / 4) + x] = 0xffd02030;
    /* The flip completion is consumed before submitting the one small clip. */
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = crtc_id, .fb_id = fb.fb_id, .flags = DRM_MODE_PAGE_FLIP_EVENT,
        .user_data = 0x1234,
    };
    call(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
    struct pollfd waiter = {.fd = fd, .events = POLLIN};
    assert(poll(&waiter, 1, 1000) == 1 && waiter.revents == POLLIN);
    struct drm_event_vblank event;
    assert(read(fd, &event, sizeof(event)) == sizeof(event));
    assert(event.base.type == DRM_EVENT_FLIP_COMPLETE && event.user_data == 0x1234 &&
           event.crtc_id == crtc_id);
    assert(read(fd, &event, sizeof(event)) == -1 && errno == EAGAIN);
    for (unsigned y = clip.y1; y < clip.y2; ++y)
        for (unsigned x = clip.x1; x < clip.x2; ++x)
            pixels[y * (dumb.pitch / 4) + x] = 0xff10d060;
    call(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    verify_damage(fd, pixels, dumb.pitch);

    verify_bo_mapping(fd);
    if (pause_for_capture) {
        puts("SVGA_SCREENSHOT_READY");
        char resume;
        assert(read(STDIN_FILENO, &resume, 1) == 1);
    }
    call(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
    call(fd, DRM_IOCTL_MODE_GETCRTC, &readback);
    assert(!readback.mode_valid);
    /* Last-scanout release must leave the VMware transport usable. */
    if (expect_3d)
        call(fd, VMW_GET_PARAM, &param);
    assert(munmap(pixels, dumb.size) == 0);
    call(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb.handle);
    assert(close(fd) == 0);
    puts("SVGA_KMS_PASS");
    return 0;
}
