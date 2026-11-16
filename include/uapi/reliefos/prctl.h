#ifndef RELIEFOS_UAPI_PRCTL_H
#define RELIEFOS_UAPI_PRCTL_H

/* ReliefNT prctl extensions. These do not change Linux personality values.
 * SET takes 0 (native) or 1 (Linux) in arg2; arg3..arg5 must be zero.
 * GET returns 0 or 1 and requires arg2..arg5 to be zero.
 * The calling task's uname sysname selection is copied by fork/clone and
 * preserved by exec. It changes no other uname fields or kernel interfaces.
 * No privilege is required; changing one task never changes another task.
 */
#define RELIEFOS_PR_SET_UNAME_LINUX 0x52550001
#define RELIEFOS_PR_GET_UNAME_LINUX 0x52550002

#endif
