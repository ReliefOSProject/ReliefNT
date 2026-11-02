/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

struct fsnotify_instance *fsnotify_instances;
static uint32_t rename_cookie;

/** @brief Create a charged instance and reserve its allocation-free overflow.
 * @param uid Real UID charged for the instance and its watches.
 * @param wake Optional nonblocking callback; it must not reenter the core.
 * @param context Borrowed callback context, live through the final put.
 * @param out Required owned instance output, cleared on failure.
 * @return Zero, -EMFILE on quota exhaustion, -ENOMEM or -EINVAL. The caller
 * holds the execution writer; no backend, inode, or path reference is acquired.
 */
int fsnotify_create(uint32_t uid,void (*wake)(void *),void *context,
                    struct fsnotify_instance **out)
{
    if(!out)return -EINVAL;
    *out=NULL;
    struct fsnotify_account *account=NULL;
    int rc=fsnotify_account_acquire(uid,&account);if(rc)return rc;
    struct fsnotify_instance *instance=kernel_malloc(sizeof(*instance));
    if(!instance){fsnotify_account_release(account);return -ENOMEM;}
    struct fsnotify_event *overflow=kernel_malloc(sizeof(*overflow));
    if(!overflow){kernel_free(instance);fsnotify_account_release(account);return -ENOMEM;}
    *overflow=(struct fsnotify_event){.event={.wd=-1,.mask=IN_Q_OVERFLOW}};
    *instance=(struct fsnotify_instance){.account=account,.overflow=overflow,
        .references=1,.next_wd=1,.queue_limit=fsnotify_limit_get(FSNOTIFY_LIMIT_QUEUED_EVENTS),
        .wake=wake,.wake_context=context,.next=fsnotify_instances};
    fsnotify_instances=instance;*out=instance;return 0;
}

/** @brief Retain a live open-file notification instance.
 * @param instance Borrowed live instance, or NULL. A saturated reference count
 * stays permanently saturated so it cannot wrap into premature destruction.
 * Execution-writer ownership is required; no backend reference is acquired.
 */
void fsnotify_get(struct fsnotify_instance *instance)
{
    if(instance && instance->references!=UINT32_MAX)++instance->references;
}

/** @brief Destroy all owned resources when the last open-file reference ends.
 * @param instance Owned reference, or NULL; consumed unless its count saturated.
 * The caller holds the execution writer. Watch keys are weak and need no puts.
 */
void fsnotify_put(struct fsnotify_instance *instance)
{
    if(!instance || !instance->references || instance->references==UINT32_MAX)return;
    if(--instance->references)return;
    struct fsnotify_instance **link=&fsnotify_instances;
    while(*link && *link!=instance)link=&(*link)->next;
    if(*link)*link=instance->next;
    while(instance->watches)fsnotify_watch_remove(instance,&instance->watches,false);
    fsnotify_queue_clear(instance);
    kernel_free(instance->overflow);
    fsnotify_account_release(instance->account);
    kernel_free(instance);
}

/** @brief Generate a nonzero global rename event cookie.
 * @return Next cookie, skipping zero on wrap. Execution writer is required;
 * this scalar has no ownership or backend lifetime dependency.
 */
uint32_t fsnotify_next_cookie(void)
{
    if(!++rename_cookie)++rename_cookie;
    return rename_cookie;
}
