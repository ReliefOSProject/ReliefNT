/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_MKSSTAT_H
#define RELIEFNT_SVGA_MKSSTAT_H

#include "host.h"

#define VMW_MKSSTAT_RESET DRM_IO(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_RESET)
#define VMW_MKSSTAT_ADD DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_ADD, struct drm_vmw_mksstat_add_arg)
#define VMW_MKSSTAT_REMOVE DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_MKSSTAT_REMOVE, struct drm_vmw_mksstat_remove_arg)

/* Entries own the descriptor and its page references until REMOVE, RESET,
 * final close of the registering DRM file, failed copyout, or device stop. */
int svga_mksstat_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data);
void svga_mksstat_rollback(struct drm_file *file, uint64_t id);
void svga_mksstat_file_release(struct drm_file *file);
void svga_mksstat_fini(void);

#endif
