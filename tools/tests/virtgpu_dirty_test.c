/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/video/drm/virtio/kms_fb.c"

static struct vg_resource resource;
static struct vg_kms_fb screen = {.id = 1, .width = 1280, .height = 800};
static struct drm_file owner = {.virtio_fbs = &screen};
static struct virtio_gpu_rect submitted[8];
static unsigned submits, fail_at, allocations;

void *kernel_malloc(size_t bytes) { ++allocations; return malloc(bytes); }
void kernel_free(void *value) { free(value); }
int user_copy_from_task(struct task *task, void *to, uint64_t from, uint64_t bytes)
{
    assert(!task);
    if (from == 1) return -EFAULT;
    memcpy(to, (const void *)(uintptr_t)from, bytes);
    return 0;
}
struct vg_resource *drm_bo_virtio(const struct drm_bo *bo)
{ assert(bo == screen.bo); return &resource; }
int vg_resource_flush(struct drm_bo *bo, struct virtio_gpu_rect rect)
{
    assert(bo == screen.bo && submits < sizeof(submitted) / sizeof(submitted[0]));
    submitted[submits++] = rect;
    return submits == fail_at ? -EIO : 0;
}

static int submit(struct drm_clip_rect *clips, unsigned count)
{
    struct drm_mode_fb_dirty_cmd req = {.fb_id = screen.id,
        .num_clips = count, .clips_ptr = (uintptr_t)clips};
    submits = 0;
    return dirty(NULL, &owner, &req);
}

static void expect(unsigned index, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    assert(index < submits);
    const struct virtio_gpu_rect *r = &submitted[index];
    assert(r->x == x && r->y == y && r->width == w && r->height == h);
}

int main(void)
{
    struct drm_clip_rect sparse[] = {{32,40,64,64}, {1216,736,1248,760}};
    assert(!submit(sparse, 2));
    if (submits != 2) fprintf(stderr, "sparse update submitted %u rectangle(s), expected 2\n", submits);
    assert(submits == 2 && !allocations);
    expect(0, 32, 40, 32, 24);
    expect(1, 1216, 736, 32, 24);

    struct drm_clip_rect nearby[] = {{32,40,64,64}, {68,40,100,64}};
    assert(!submit(nearby, 2) && submits == 1);
    expect(0, 32, 40, 68, 24);
    struct drm_clip_rect overlap[] = {{32,40,64,64}, {33,41,63,63}, {1216,736,1248,760}};
    assert(!submit(overlap, 3) && submits == 1);
    expect(0, 32, 40, 1216, 720);

    struct drm_clip_rect many[] = {{32,40,64,64}, {1216,736,1248,760},
                                  {200,200,201,201}, {400,400,401,401}, {600,600,601,601}};
    assert(!submit(many, 5) && submits == 1 && allocations == 1);
    expect(0, 32, 40, 1216, 720);
    many[4].x2 = 1281;
    assert(submit(many, 5) == -EINVAL && !submits);
    assert(submit((void *)1, 2) == -EFAULT && !submits);

    resource.virgl = true;
    assert(!submit(sparse, 2) && submits == 1);
    expect(0, 32, 40, 1216, 720);
    resource.virgl = false;
    fail_at = 1;
    assert(submit(sparse, 2) == -EIO && submits == 1);
    fail_at = 0;
    assert(!submit(NULL, 0) && submits == 1);
    expect(0, 0, 0, 1280, 800);
    puts("VirtIO DIRTYFB: sparse uploads, coalescing, validation and error propagation passed");
    return 0;
}
