/* Direct synchronous RAM-disk codec fixtures have no scheduler transaction.
 * Model scoped flag save/restore; native continuation tests link the actual
 * storage_state.c and registered provider rather than these codec seams. */
#ifndef RELIEFNT_STORAGE_CODEC_CONTEXT_FIXTURE_H
#define RELIEFNT_STORAGE_CODEC_CONTEXT_FIXTURE_H
#include <reliefnt/storage_context.h>
static struct storage_io_context codec_io_context;
void storage_kernel_context_enter(struct storage_io_context *saved)
{
    *saved = codec_io_context;
    codec_io_context = (struct storage_io_context){.write_started = true};
}
void storage_kernel_context_leave(const struct storage_io_context *saved)
{
    codec_io_context = *saved;
}
#endif
