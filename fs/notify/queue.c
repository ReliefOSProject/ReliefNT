/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Append a prepared queue record and notify the owning descriptor.
 * @param instance Borrowed live instance that takes the queued record.
 * @param event Prepared owned record, or the instance's reserved overflow record.
 * Execution writer is required; wake callbacks must not reenter the core.
 */
static void fsnotify_queue_append(struct fsnotify_instance *instance,struct fsnotify_event *event)
{
    event->next=NULL;
    if(instance->tail)instance->tail->next=event;
    else instance->head=event;
    instance->tail=event;
    instance->bytes+=sizeof(struct inotify_event)+event->event.len;
    if(instance->wake)instance->wake(instance->wake_context);
}

/** @brief Queue the preallocated overflow sentinel at most once until consumed.
 * @param instance Borrowed live instance owning the reserved record.
 * Execution writer is required; no allocation or ownership transfer occurs.
 */
static void fsnotify_queue_overflow(struct fsnotify_instance *instance)
{
    if(instance->overflow_queued)return;
    instance->overflow_queued=true;
    fsnotify_queue_append(instance,instance->overflow);
}

/** @brief Determine a safe basename size; reject names longer than Linux NAME_MAX.
 * @param name Borrowed NUL-terminated basename, or NULL for no name.
 * @param length Required output byte count excluding NUL.
 * @return True when the name fits, false when it exceeds the limit. Called
 * under the execution writer; the input remains owned by the caller.
 */
static bool fsnotify_name_length(const char *name,size_t *length)
{
    *length=0;
    if(!name)return true;
    while(*length<=LINUX_NAME_MAX && name[*length])++*length;
    return *length<=LINUX_NAME_MAX;
}

/** @brief Check whether a new event equals the adjacent unread event.
 * @param tail Borrowed last queued event, or NULL.
 * @param wd Watch descriptor for the candidate.
 * @param mask Candidate event mask.
 * @param cookie Candidate rename cookie.
 * @param name Borrowed candidate name, nullable when length is zero.
 * @param length Candidate name bytes excluding NUL.
 * @param padded Candidate ABI name-field byte length.
 * @return True when all fields and the name match. The execution writer keeps
 * the borrowed tail and name stable; no reference or allocation is acquired.
 */
static bool fsnotify_queue_duplicate(const struct fsnotify_event *tail,int wd,uint32_t mask,
                                     uint32_t cookie,const char *name,size_t length,uint32_t padded)
{
    if(!tail || tail->event.wd!=wd || tail->event.mask!=mask ||
       tail->event.cookie!=cookie || tail->event.len!=padded)return false;
    for(size_t n=0;n<length;++n)if(tail->event.name[n]!=name[n])return false;
    return !length || !tail->event.name[length];
}

/** @brief Append a whole event, falling back to reserved overflow on any loss.
 * @param instance Borrowed live target queue.
 * @param wd Watch descriptor associated with the event.
 * @param mask Event bits and optional directory marker.
 * @param cookie Rename cookie or zero.
 * @param name Borrowed optional basename copied before return.
 * Execution writer is required. Allocation or queue exhaustion is reported by
 * the reserved overflow record and never fails the originating operation.
 */
void fsnotify_queue_event(struct fsnotify_instance *instance,int wd,uint32_t mask,
                          uint32_t cookie,const char *name)
{
    size_t length;
    if(!fsnotify_name_length(name,&length)){fsnotify_queue_overflow(instance);return;}
    uint32_t padded=length ? (uint32_t)((length+1+15)&~(size_t)15) : 0;
    if(fsnotify_queue_duplicate(instance->tail,wd,mask,cookie,name,length,padded))return;
    if(instance->queued>=instance->queue_limit){fsnotify_queue_overflow(instance);return;}
    struct fsnotify_event *event=kernel_malloc(sizeof(*event)+padded);
    if(!event){fsnotify_queue_overflow(instance);return;}
    event->event.wd=wd;event->event.mask=mask;event->event.cookie=cookie;
    event->event.len=padded;
    if(padded) {
        __builtin_memset(event->event.name,0,padded);
        __builtin_memcpy(event->event.name,name,length);
    }
    ++instance->queued;
    fsnotify_queue_append(instance,event);
}

/** @brief Return the first complete record without modifying the queue.
 * @param instance Borrowed instance, or NULL.
 * @param out Required output for a borrowed record, cleared if the queue is empty.
 * @return Whole-record byte length, or zero for invalid/empty input. Execution
 * writer is required; the record is valid only until the next queue mutation.
 */
size_t fsnotify_peek(struct fsnotify_instance *instance,const struct inotify_event **out)
{
    if(!out)return 0;
    *out=NULL;
    if(!instance || !instance->head)return 0;
    *out=&instance->head->event;
    return sizeof(struct inotify_event)+instance->head->event.len;
}

/** @brief Consume one whole queued record, recycling the overflow reserve.
 * @param instance Borrowed live instance, or NULL. Execution writer is
 * required; ordinary records are freed and the reserved overflow is retained.
 */
void fsnotify_pop(struct fsnotify_instance *instance)
{
    if(!instance || !instance->head)return;
    struct fsnotify_event *event=instance->head;
    instance->head=event->next;
    if(!instance->head)instance->tail=NULL;
    instance->bytes-=sizeof(struct inotify_event)+event->event.len;
    if(event==instance->overflow)instance->overflow_queued=false;
    else {--instance->queued;kernel_free(event);}
}

/** @brief Consume every queued record without releasing the overflow reserve.
 * @param instance Borrowed live instance. Execution writer is required;
 * record ownership is consumed and the instance retains its overflow reserve.
 */
void fsnotify_queue_clear(struct fsnotify_instance *instance)
{
    while(instance->head)fsnotify_pop(instance);
}

/** @brief Return the exact byte count currently readable.
 * @param instance Borrowed instance, or NULL.
 * @return Queued whole-record bytes, or zero for NULL. Execution writer is
 * required; this scalar query does not transfer or retain ownership.
 */
size_t fsnotify_pending_bytes(const struct fsnotify_instance *instance)
{
    return instance ? instance->bytes : 0;
}

/** @brief Return whether the instance currently has a complete record.
 * @param instance Borrowed instance, or NULL.
 * @return True when a record is readable. Execution writer is required;
 * the query takes no reference and does not modify the queue.
 */
bool fsnotify_ready(const struct fsnotify_instance *instance)
{
    return instance && instance->head;
}
