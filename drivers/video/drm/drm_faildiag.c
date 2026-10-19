#include "drm_internal.h"

#include <linux/errno.h>
#include <reliefnt/console.h>

#define DRM_FAILDIAG_RECORD_LIMIT 16u

static uint32_t drm_faildiag_enabled;
static uint32_t drm_faildiag_record_count;

bool drm_faildiag_is_enabled(void)
{
    return __atomic_load_n(&drm_faildiag_enabled, __ATOMIC_ACQUIRE) != 0;
}

static bool drm_faildiag_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == '\v' || value == '\f';
}

/**
 * @brief Reset the failure-record gate and enable it for one exact boot token.
 * @param cmdline Raw boot command line; NULL disables diagnostic output.
 * @return No value; the shared record count starts at zero for this boot.
 */
void drm_faildiag_configure(const char *cmdline)
{
    static const char option[] = "drm.faildiag=1";
    const uint32_t option_length = (uint32_t)(sizeof(option) - 1u);
    bool enabled = false;

    __atomic_store_n(&drm_faildiag_enabled, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&drm_faildiag_record_count, 0u, __ATOMIC_RELAXED);
    for (const char *cursor = cmdline; cursor && *cursor;) {
        while (*cursor && drm_faildiag_space(*cursor)) ++cursor;
        const char *start = cursor;
        while (*cursor && !drm_faildiag_space(*cursor)) ++cursor;
        if ((uint32_t)(cursor - start) == option_length) {
            uint32_t index = 0;
            while (index < option_length && start[index] == option[index]) ++index;
            if (index == option_length) {
                enabled = true;
                break;
            }
        }
    }
    __atomic_store_n(&drm_faildiag_enabled, enabled ? 1u : 0u,
                     __ATOMIC_RELEASE);
}

static bool drm_faildiag_reserve_record(void)
{
    uint32_t count = __atomic_load_n(&drm_faildiag_record_count,
                                     __ATOMIC_RELAXED);
    while (count < DRM_FAILDIAG_RECORD_LIMIT) {
        if (__atomic_compare_exchange_n(&drm_faildiag_record_count, &count,
                count + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

/**
 * @brief Emit one capped, allocation-free record for ENOMEM or EINVAL.
 * @param context Stack-owned ioctl identity and duplicate-suppression state.
 * @param error Original negative errno returned by the operation.
 * @param stage Static kernel stage tag; never caller-provided text.
 * @param fields Static field names and numeric snapshots, or NULL.
 * @param field_count Number of valid fields, capped at the fixed record width.
 * @return No value; the return path and its errno are never changed.
 */
void drm_faildiag_record(struct drm_faildiag_context *context, int error,
                         const char *stage,
                         const struct drm_faildiag_field *fields,
                         uint32_t field_count)
{
    const char *names[DRM_FAILDIAG_FIELD_MAX];
    uint64_t values[DRM_FAILDIAG_FIELD_MAX];
    if (!context || !stage ||
        !drm_faildiag_is_enabled() ||
        (error != -LINUX_ENOMEM && error != -LINUX_EINVAL))
        return;

    context->attempted = true;
    if (!drm_faildiag_reserve_record()) return;
    if (field_count > DRM_FAILDIAG_FIELD_MAX)
        field_count = DRM_FAILDIAG_FIELD_MAX;
    for (uint32_t i = 0; i < DRM_FAILDIAG_FIELD_MAX; ++i) {
        names[i] = i < field_count && fields && fields[i].name
            ? fields[i].name : "-";
        values[i] = i < field_count && fields ? fields[i].value : 0;
    }

    console_error("[drm:faildiag] pid=%u ioctl=0x%08x handler=%u/%s "
                  "stage=%s errno=%d %s=%llu %s=%llu %s=%llu %s=%llu "
                  "%s=%llu %s=%llu %s=%llu %s=%llu\n",
        context->pid, context->command, context->handler,
        context->handler_name ? context->handler_name : "unknown", stage,
        error, names[0], (unsigned long long)values[0],
        names[1], (unsigned long long)values[1],
        names[2], (unsigned long long)values[2],
        names[3], (unsigned long long)values[3],
        names[4], (unsigned long long)values[4],
        names[5], (unsigned long long)values[5],
        names[6], (unsigned long long)values[6],
        names[7], (unsigned long long)values[7]);
}
