/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * <linux/patchkey.h> -- definition of _PATCHKEY macro
 *
 * Copyright (C) 2005 Stuart Brady
 *
 * This exists because awe_voice.h defined its own _PATCHKEY and it wasn't
 * clear whether removing this would break anything in userspace.
 *
 * The exported ReliefOS copy is self-contained so headers_install can compile
 * every whitelisted UAPI header independently.  OSS still includes it through
 * the canonical indirect path.
 */

#ifndef _UAPI_LINUX_PATCHKEY_H
#define _UAPI_LINUX_PATCHKEY_H

/* Source: Linux v6.14 include/uapi/linux/patchkey.h (SHA256 pinned in the
 * audio ABI handoff); ReliefOS removes the direct-include guard for export. */

#if !defined(__KERNEL__)
#if defined(__BYTE_ORDER)
#  if __BYTE_ORDER == __BIG_ENDIAN
#    define _PATCHKEY(id) (0xfd00|id)
#  elif __BYTE_ORDER == __LITTLE_ENDIAN
#    define _PATCHKEY(id) ((id<<8)|0x00fd)
#  else
#    error "could not determine byte order"
#  endif
#endif
#endif

#endif /* _UAPI_LINUX_PATCHKEY_H */
