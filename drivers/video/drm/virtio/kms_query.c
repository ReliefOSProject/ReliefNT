/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/usercopy.h>
#include <reliefnt/heap.h>
#include <reliefnt/text_stream.h>
#include <reliefnt/graphics_service.h>

/** @brief Fill a sufficiently sized query array using the ordinary usercopy boundary.
 * @param task Caller. @param address User destination. @param capacity Slots.
 * @param data Kernel elements. @param count Actual count. @param size Element bytes.
 * @return Zero or errno/retry; short capacities return counts without copying.
 */
int vg_kms_array(struct task *task,uint64_t address,uint32_t capacity,const void *data,uint32_t count,uint32_t size)
{
    if(capacity<count || !count)return 0;
    uint64_t bytes=(uint64_t)count*size;
#ifdef CONFIG_SWAP
    int rc=drm_user_prepare(task,address,bytes,true);if(rc)return rc;
#endif
    return user_copy_to_task(task,address,data,bytes)<0 ? -EFAULT : 0;
}

/** @brief Build a progressive 60 Hz virtual monitor mode with valid blanking.
 * @param mode Output. @param width Pixels. @param height Pixels. @param preferred
 * Set only for the host-advertised preferred dimensions.
 */
static void mode_create(struct drm_mode_modeinfo *mode,uint32_t width,uint32_t height,bool preferred)
{
    *mode=(struct drm_mode_modeinfo){.hdisplay=width,.hsync_start=width+48,
        .hsync_end=width+80,.htotal=width+160,.vdisplay=height,.vsync_start=height+3,
        .vsync_end=height+8,.vtotal=height+45,.vrefresh=60,
        .clock=(width+160)*(height+45)*60/1000,
        .flags=DRM_MODE_FLAG_PHSYNC|DRM_MODE_FLAG_NVSYNC,
        .type=DRM_MODE_TYPE_DRIVER|(preferred ? DRM_MODE_TYPE_PREFERRED : 0)};
    struct text_stream name={.buffer=mode->name,.capacity=sizeof(mode->name)-1};
    text_unsigned(&name,width);text_string(&name,"x");text_unsigned(&name,height);
}

/** @brief Enumerate driver-valid modes with the host's preferred dimensions first.
 * @param output Hardware output. @param modes At least ten writable elements.
 * @return Number of populated modes; disconnected outputs expose no modes.
 */
static unsigned modes_get(unsigned output,struct drm_mode_modeinfo *modes)
{
    if(!vg.display.pmodes[output].enabled)return 0;
    uint32_t width=vg.display.pmodes[output].r.width,height=vg.display.pmodes[output].r.height;
    unsigned count=0;
    if(width>=320 && height>=200 && width<=VG_MAX_WIDTH && height<=VG_MAX_HEIGHT)
        mode_create(&modes[count++],width,height,true);
    static const uint16_t sizes[][2]={{640,480},{800,600},{1024,768},{1280,720},{1280,800},
        {1440,900},{1600,900},{1920,1080},{2560,1440}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        if(sizes[i][0]==width && sizes[i][1]==height)continue;
        mode_create(&modes[count++],sizes[i][0],sizes[i][1],false);
    }
    return count;
}

/** @brief Publish a pointer-free copy for standard sysfs connector attributes.
 * @param index Hardware output. @param out Copied destination.
 * @return Zero or errno; execution ownership serializes EDID/display refresh.
 */
int vg_connector_info(uint32_t index,struct graphics_connector_info *out)
{
    if(!vg.ready || vg.fault)return -ENODEV;
    if(index>=vg.scanout_count)return -ENOENT;
    *out=(struct graphics_connector_info){.connected=vg.display.pmodes[index].enabled,
        .enabled=vg_outputs[index].enabled,.edid_size=vg.edid[index].size};
    struct drm_mode_modeinfo modes[10];out->mode_count=modes_get(index,modes);
    for(unsigned i=0;i<out->mode_count;++i){out->widths[i]=modes[i].hdisplay;out->heights[i]=modes[i].vdisplay;}
    if(out->edid_size)__builtin_memcpy(out->edid,vg.edid[index].edid,out->edid_size);
    return 0;
}

/** @brief Enumerate only file-authorized framebuffer identifiers and all CRTCs.
 * @param task Caller. @param file OFD. @param arg Copied query.
 * @return Zero or errno/retry; no persistent object pointer crosses a retry.
 */
static int resources(struct task *task,struct drm_file *file,struct drm_mode_card_res *arg)
{
    uint32_t crtcs[16],encoders[16],connectors[16];
    for(unsigned i=0;i<vg.scanout_count;++i){crtcs[i]=VG_CRTC+i;encoders[i]=VG_ENCODER+i;connectors[i]=VG_CONNECTOR+i;}
    uint32_t count=0;for(struct vg_kms_fb *fb=file->virtio_fbs;fb;fb=fb->next)++count;
    uint32_t *fbs=count ? kernel_malloc((size_t)count*4) : NULL;
    if(count && !fbs)return -ENOMEM;
    unsigned n=0;for(struct vg_kms_fb *fb=file->virtio_fbs;fb;fb=fb->next)fbs[n++]=fb->id;
    int rc=vg_kms_array(task,arg->fb_id_ptr,arg->count_fbs,fbs,count,4);
    if(!rc)rc=vg_kms_array(task,arg->crtc_id_ptr,arg->count_crtcs,crtcs,vg.scanout_count,4);
    if(!rc)rc=vg_kms_array(task,arg->encoder_id_ptr,arg->count_encoders,encoders,vg.scanout_count,4);
    if(!rc)rc=vg_kms_array(task,arg->connector_id_ptr,arg->count_connectors,connectors,vg.scanout_count,4);
    kernel_free(fbs);
    arg->count_fbs=count;arg->count_crtcs=arg->count_encoders=arg->count_connectors=vg.scanout_count;
    arg->min_width=arg->min_height=1;arg->max_width=VG_MAX_WIDTH;arg->max_height=VG_MAX_HEIGHT;
    return rc;
}

/** @brief Return connector modes, EDID/DPMS properties and fixed encoder binding.
 * @param task Caller. @param arg Copied connector query. @return Zero or errno/retry.
 */
static int connector(struct task *task,struct drm_mode_get_connector *arg)
{
    unsigned i=arg->connector_id-VG_CONNECTOR;if(i>=vg.scanout_count)return -ENOENT;
    struct drm_mode_modeinfo modes[10];unsigned count=modes_get(i,modes);
    uint32_t encoder=VG_ENCODER+i,props[]={VG_PROP_EDID,VG_PROP_DPMS};
    uint64_t values[]={vg.edid[i].size ? VG_EDID_BLOB+i : 0,vg_outputs[i].enabled ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF};
    int rc=vg_kms_array(task,arg->modes_ptr,arg->count_modes,modes,count,sizeof(*modes));
    if(!rc)rc=vg_kms_array(task,arg->encoders_ptr,arg->count_encoders,&encoder,1,4);
    if(!rc)rc=vg_kms_array(task,arg->props_ptr,arg->count_props,props,2,4);
    if(!rc)rc=vg_kms_array(task,arg->prop_values_ptr,arg->count_props,values,2,8);
    arg->count_modes=count;arg->count_encoders=1;arg->count_props=2;
    arg->encoder_id=encoder;arg->connector_type=DRM_MODE_CONNECTOR_VIRTUAL;
    arg->connector_type_id=i+1;arg->connection=vg.display.pmodes[i].enabled ? 1 : 2;
    arg->mm_width=vg.edid[i].size ? (uint32_t)vg.edid[i].edid[21]*10 : 0;
    arg->mm_height=vg.edid[i].size ? (uint32_t)vg.edid[i].edid[22]*10 : 0;
    arg->subpixel=1;return rc;
}

/** @brief Describe standard connector/plane properties without invented ABI names.
 * @param task Caller. @param arg Query. @return Zero or errno/retry.
 */
static int property(struct task *task,struct drm_mode_get_property *arg)
{
    uint64_t values[4]={0,1,2,3};unsigned count=0,enum_count=0;
    struct drm_mode_property_enum enums[4]={0};
    const char *name;
    switch(arg->prop_id) {
    case VG_PROP_EDID:name="EDID";arg->flags=DRM_MODE_PROP_BLOB|DRM_MODE_PROP_IMMUTABLE;break;
    case VG_PROP_DPMS:
        name="DPMS";arg->flags=DRM_MODE_PROP_ENUM;count=enum_count=4;
        __builtin_memcpy(enums[0].name,"On",3);__builtin_memcpy(enums[1].name,"Standby",8);
        __builtin_memcpy(enums[2].name,"Suspend",8);__builtin_memcpy(enums[3].name,"Off",4);break;
    case VG_PROP_PLANE_TYPE:
        name="type";arg->flags=DRM_MODE_PROP_ENUM|DRM_MODE_PROP_IMMUTABLE;count=enum_count=1;
        values[0]=1;__builtin_memcpy(enums[0].name,"Primary",8);break;
    default:return -ENOENT;
    }
    for(unsigned i=0;i<enum_count;++i)enums[i].value=values[i];
    int rc=vg_kms_array(task,arg->values_ptr,arg->count_values,values,count,8);
    if(!rc)rc=vg_kms_array(task,arg->enum_blob_ptr,arg->count_enum_blobs,enums,enum_count,sizeof(*enums));
    __builtin_memset(arg->name,0,sizeof(arg->name));__builtin_memcpy(arg->name,name,__builtin_strlen(name));
    arg->count_values=count;arg->count_enum_blobs=enum_count;return rc;
}

/** @brief Return property values for existing CRTC, connector or primary plane.
 * @param task Caller. @param arg Copied query. @return Zero or errno/retry.
 */
static int object_properties(struct task *task,struct drm_mode_obj_get_properties *arg)
{
    uint32_t properties[2];uint64_t values[2];unsigned count=0;
    if(arg->obj_type==DRM_MODE_OBJECT_CONNECTOR && arg->obj_id-VG_CONNECTOR<vg.scanout_count) {
        unsigned i=arg->obj_id-VG_CONNECTOR;count=2;
        properties[0]=VG_PROP_EDID;properties[1]=VG_PROP_DPMS;
        values[0]=vg.edid[i].size ? VG_EDID_BLOB+i : 0;values[1]=vg_outputs[i].enabled ? 0 : 3;
    } else if(arg->obj_type==DRM_MODE_OBJECT_PLANE && arg->obj_id-VG_PLANE<vg.scanout_count) {
        count=1;properties[0]=VG_PROP_PLANE_TYPE;values[0]=1;
    } else if(!(arg->obj_type==DRM_MODE_OBJECT_CRTC && arg->obj_id-VG_CRTC<vg.scanout_count))return -ENOENT;
    int rc=vg_kms_array(task,arg->props_ptr,arg->count_props,properties,count,4);
    if(!rc)rc=vg_kms_array(task,arg->prop_values_ptr,arg->count_props,values,count,8);
    arg->count_props=count;return rc;
}

/** @brief Dispatch immutable KMS queries using Linux structure/count conventions.
 * @param task Caller. @param file OFD. @param command Canonical selector.
 * @param data Kernel argument. @return Zero/errno/retry or -VG_IOCTL_UNOWNED if unowned.
 */
int vg_kms_query(struct task *task,struct drm_file *file,uint32_t command,void *data)
{
    switch(command) {
    case DRM_IOCTL_MODE_GETRESOURCES:return resources(task,file,data);
    case DRM_IOCTL_MODE_GETCONNECTOR:return connector(task,data);
    case DRM_IOCTL_MODE_GETPROPERTY:return property(task,data);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:return object_properties(task,data);
    case DRM_IOCTL_MODE_GETPROPBLOB: {
        struct drm_mode_get_blob *arg=data;unsigned i=arg->blob_id-VG_EDID_BLOB;
        if(i>=vg.scanout_count || !vg.edid[i].size)return -ENOENT;
        int rc=vg_kms_array(task,arg->data,arg->length,vg.edid[i].edid,vg.edid[i].size,1);
        arg->length=vg.edid[i].size;return rc;
    }
    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_crtc *arg=data;unsigned i=arg->crtc_id-VG_CRTC;
        if(i>=vg.scanout_count)return -ENOENT;
        struct vg_output *out=&vg_outputs[i];arg->fb_id=out->fb ? out->fb->id : 0;
        arg->x=out->x;arg->y=out->y;arg->gamma_size=0;arg->mode_valid=out->enabled;
        arg->mode=out->mode;return 0;
    }
    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *arg=data;unsigned i=arg->encoder_id-VG_ENCODER;
        if(i>=vg.scanout_count)return -ENOENT;
        arg->encoder_type=DRM_MODE_ENCODER_VIRTUAL;arg->crtc_id=vg_outputs[i].enabled ? VG_CRTC+i : 0;
        arg->possible_crtcs=1u<<i;arg->possible_clones=0;return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res *arg=data;uint32_t planes[16];
        unsigned count=file->universal_planes ? vg.scanout_count : 0;
        for(unsigned i=0;i<count;++i)planes[i]=VG_PLANE+i;
        int rc=vg_kms_array(task,arg->plane_id_ptr,arg->count_planes,planes,count,4);
        arg->count_planes=count;return rc;
    }
    case DRM_IOCTL_MODE_GETPLANE: {
        struct drm_mode_get_plane *arg=data;unsigned i=arg->plane_id-VG_PLANE;
        if(i>=vg.scanout_count)return -ENOENT;
        uint32_t formats[]={VG_XRGB8888,VG_ARGB8888};
        int rc=vg_kms_array(task,arg->format_type_ptr,arg->count_format_types,formats,2,4);
        arg->count_format_types=2;arg->possible_crtcs=1u<<i;arg->gamma_size=0;
        arg->crtc_id=vg_outputs[i].enabled ? VG_CRTC+i : 0;
        arg->fb_id=vg_outputs[i].fb ? vg_outputs[i].fb->id : 0;return rc;
    }
    default:return -VG_IOCTL_UNOWNED;
    }
}
