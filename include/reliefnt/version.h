/*
 * ReliefOS version interface: declares exported system-version metadata.
 * Provides build identity to kernel and userland information services.
 */
#ifndef RELIEFNT_VERSION_H
#define RELIEFNT_VERSION_H

#include <reliefos/system_abi.h>

const struct reliefos_system_info *reliefnt_system_info(void);

#endif
