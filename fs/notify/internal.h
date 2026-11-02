/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FSNOTIFY_INTERNAL_H
#define RELIEFNT_FSNOTIFY_INTERNAL_H

#include <reliefnt/fsnotify.h>
#include <reliefnt/mount.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>
#include <linux/limits.h>

struct fsnotify_account {
    struct fsnotify_account *next;
    uint32_t uid,instances,watches;
};
struct fsnotify_watch {
    struct fsnotify_watch *next;
    struct fsnotify_key key;
    uint32_t mask;
    int wd;
};
struct fsnotify_event {
    struct fsnotify_event *next;
    struct inotify_event event;
};
struct fsnotify_instance {
    struct fsnotify_instance *next;
    struct fsnotify_account *account;
    struct fsnotify_watch *watches;
    struct fsnotify_event *head,*tail,*overflow;
    size_t bytes;
    uint32_t references,queued,queue_limit;
    int next_wd;
    bool overflow_queued;
    void (*wake)(void *);
    void *wake_context;
};
extern struct fsnotify_instance *fsnotify_instances;

/** @brief Find or create the charged UID account and reserve an instance.
 * @param uid Real UID charged for the instance.
 * @param out Required borrowed account output, cleared on failure.
 * @return Zero, -EMFILE or -ENOMEM. Execution writer is required; a successful
 * instance charge keeps the account live until fsnotify_account_release().
 */
int fsnotify_account_acquire(uint32_t uid,struct fsnotify_account **out);
/** @brief Release an instance charge and its empty account.
 * @param account Charged account or NULL; final instance has no live watches.
 * Execution writer is required; the final release consumes the account.
 */
void fsnotify_account_release(struct fsnotify_account *account);
/** @brief Admit one additional watch against the current UID policy.
 * @param account Borrowed account kept live by an instance charge.
 * @return Zero or -ENOSPC. Execution writer is required; a successful charge
 * must be balanced by fsnotify_account_watch_release().
 */
int fsnotify_account_watch_acquire(struct fsnotify_account *account);
/** @brief Release one UID watch charge.
 * @param account Borrowed charged account, or NULL.
 * Execution writer is required; no account ownership is transferred.
 */
void fsnotify_account_watch_release(struct fsnotify_account *account);
/** @brief Append an event or a reserved overflow marker when delivery is lost.
 * @param instance Borrowed live target queue.
 * @param wd Watch descriptor.
 * @param mask Event mask.
 * @param cookie Rename cookie or zero.
 * @param name Borrowed optional basename copied into the event.
 * Execution writer is required; allocation failure never fails the operation.
 */
void fsnotify_queue_event(struct fsnotify_instance *instance,int wd,uint32_t mask,
                          uint32_t cookie,const char *name);
/** @brief Release all queued records, leaving the reserved overflow owned.
 * @param instance Borrowed live queue.
 * Execution writer is required; the instance keeps its overflow reserve.
 */
void fsnotify_queue_clear(struct fsnotify_instance *instance);
/** @brief Compare provider identity, then actual inode identity.
 * @param a First borrowed weak key, or NULL.
 * @param b Second borrowed weak key, or NULL.
 * @return True if both identify the same inode. Execution writer keeps the
 * provider code/context live; neither key acquires an inode or mount pin.
 */
bool fsnotify_key_same(const struct fsnotify_key *a,const struct fsnotify_key *b);
/** @brief Retire one linked watch, optionally notifying its descriptor.
 * @param instance Borrowed owning instance.
 * @param link Required pointer to the watch's list link.
 * @param ignored True to append IN_IGNORED.
 * Execution writer is required. The watch and its charge are consumed; its
 * weak inode identity owns no provider, mount, path, or inode reference.
 */
void fsnotify_watch_remove(struct fsnotify_instance *instance,
                           struct fsnotify_watch **link,bool ignored);

#endif
