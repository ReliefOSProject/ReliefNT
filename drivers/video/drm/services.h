/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DRM_SERVICES_H
#define RELIEFNT_DRM_SERVICES_H
/** @brief Publish DRM after discovering the independently bound boot framebuffer.
 * @return Zero or discovery/registration errno, with no table left on failure.
 * Init task context after heap initialization; no provider self-reference.
 */
int drm_core_services_start(void);
/** @brief Detach the exact idle DRM table before provider code can disappear.
 * Task/exit context after external and asynchronous owners drain; the module
 * cleanup ledger may already have removed this registration.
 */
void drm_core_services_stop(void);
#endif
