/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

static struct fsnotify_account *accounts;
static uint32_t max_instances=128,max_watches=8192,max_queued=16384;

/** @brief Read the configured quota or per-instance queue capacity.
 * @param limit Resource selector.
 * @return Current limit, or zero for an invalid selector. The caller holds the
 * execution writer; the result is a scalar and transfers no ownership.
 */
uint32_t fsnotify_limit_get(enum fsnotify_limit limit)
{
    switch(limit) {
    case FSNOTIFY_LIMIT_INSTANCES:return max_instances;
    case FSNOTIFY_LIMIT_WATCHES:return max_watches;
    case FSNOTIFY_LIMIT_QUEUED_EVENTS:return max_queued;
    default:return 0;
    }
}

/** @brief Update a quota without changing any already charged resource.
 * @param limit Resource selector.
 * @param value New limit; zero is permitted.
 * @return Zero or -EINVAL for an invalid selector. The execution writer is
 * required; existing instances and watches remain charged and allocated.
 */
int fsnotify_limit_set(enum fsnotify_limit limit,uint32_t value)
{
    switch(limit) {
    case FSNOTIFY_LIMIT_INSTANCES:max_instances=value;return 0;
    case FSNOTIFY_LIMIT_WATCHES:max_watches=value;return 0;
    case FSNOTIFY_LIMIT_QUEUED_EVENTS:max_queued=value;return 0;
    default:return -EINVAL;
    }
}

/** @brief Reserve an instance against the real UID's current limit.
 * @param uid Real UID to charge.
 * @param out Required borrowed account output, cleared on failure.
 * @return Zero, -EMFILE on quota exhaustion, or -ENOMEM. Execution writer is
 * required; the acquired charge keeps the account live until release.
 */
int fsnotify_account_acquire(uint32_t uid,struct fsnotify_account **out)
{
    struct fsnotify_account *account=accounts;
    *out=NULL;
    while(account && account->uid!=uid)account=account->next;
    if(account && account->instances>=max_instances)return -EMFILE;
    if(!account) {
        if(!max_instances)return -EMFILE;
        account=kernel_malloc(sizeof(*account));
        if(!account)return -ENOMEM;
        *account=(struct fsnotify_account){.uid=uid,.next=accounts};
        accounts=account;
    }
    ++account->instances;*out=account;return 0;
}

/** @brief Drop one instance charge and retire unused per-UID accounting.
 * @param account Charged account or NULL; its final instance release frees it.
 * Execution writer is required, and all its final watches must already retire.
 */
void fsnotify_account_release(struct fsnotify_account *account)
{
    if(!account || !account->instances)return;
    if(--account->instances)return;
    struct fsnotify_account **link=&accounts;
    while(*link && *link!=account)link=&(*link)->next;
    if(*link)*link=account->next;
    kernel_free(account);
}

/** @brief Check and charge one watch against the UID's current limit.
 * @param account Borrowed live account protected by an instance charge.
 * @return Zero or -ENOSPC. The caller holds the execution writer and must
 * balance a successful charge with fsnotify_account_watch_release().
 */
int fsnotify_account_watch_acquire(struct fsnotify_account *account)
{
    if(account->watches>=max_watches)return -ENOSPC;
    ++account->watches;return 0;
}

/** @brief Return one watch charge to its UID.
 * @param account Borrowed charged account, or NULL. Execution writer is
 * required; this releases the charge without transferring account ownership.
 */
void fsnotify_account_watch_release(struct fsnotify_account *account)
{
    if(account && account->watches)--account->watches;
}
