#ifndef RELIEFNT_DRM_VMWGFX_EXECBUF_PARSER_H
#define RELIEFNT_DRM_VMWGFX_EXECBUF_PARSER_H

#include <stdint.h>

#define DRM_VMW_EXECBUF_MAX_COMMAND_BYTES (64u * 1024u)

typedef int (*drm_vmwgfx_packet_visitor)(uint32_t command,
                                         const uint8_t *body,
                                         uint32_t body_bytes,
                                         uint32_t packet_offset,
                                         void *context);

/* Pure first-pass validation. No resource lookup or device state is touched. */
int drm_vmwgfx_validate_stream(const void *stream, uint32_t bytes,
                               uint32_t *packet_count);
/* Validate and walk immutable packet bytes. Visitors run only after each
 * individual packet has passed structural validation; a visitor error stops
 * the walk and is returned unchanged. */
int drm_vmwgfx_walk_stream(const void *stream, uint32_t bytes,
                           uint32_t *packet_count,
                           drm_vmwgfx_packet_visitor visitor,
                           void *context);

#endif
