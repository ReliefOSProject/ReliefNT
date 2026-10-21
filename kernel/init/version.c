/*
 * ReliefOS kernel version module: exposes build and release metadata.
 * Publishes the system information consumed by diagnostics and userland.
 */
#include <reliefnt/version.h>

#include <generated/build_info.h>

#if defined(__x86_64__)
#define RELIEFNT_ARCHITECTURE "x86_64"
#elif defined(__aarch64__)
#define RELIEFNT_ARCHITECTURE "aarch64"
#else
#define RELIEFNT_ARCHITECTURE "unknown"
#endif

static const struct reliefos_system_info system_info = {
    .kernel_name = RELIEFOS_KERNEL_NAME,
    .kernel_version = RELIEFOS_KERNEL_VERSION,
    .reserved_subsystem_name = "",
    .build_time = RELIEFOS_BUILD_TIME,
    .copyright = RELIEFOS_COPYRIGHT,
    .version_major = RELIEFOS_KERNEL_VERSION_MAJOR,
    .version_minor = RELIEFOS_KERNEL_VERSION_MINOR,
    .version_patch = RELIEFOS_KERNEL_VERSION_PATCH,
    .reserved_version = 0,
    .copyright_year = RELIEFOS_COPYRIGHT_YEAR,
    .architecture = RELIEFNT_ARCHITECTURE,
};

/**
 * @brief Return the build-time system information record.
 */
const struct reliefos_system_info *reliefnt_system_info(void)
{
    return &system_info;
}
