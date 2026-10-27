/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DRM_OWNER_FIXTURE_H
#define RELIEFNT_DRM_OWNER_FIXTURE_H
#include <reliefnt/module.h>
extern struct module drm_test_native_owner;
/* Select a real lifecycle owner for the actual production DRM objects without
 * replacing their allocations, cleanup, service broker or module pin methods. */
#undef THIS_MODULE
#define THIS_MODULE (&drm_test_native_owner)
#endif
