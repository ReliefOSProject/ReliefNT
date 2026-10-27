/* Non-ELF fixtures do not create executable backing sources. A non-NULL
 * source is a test-boundary error, never simulated lifetime or I/O proof. */
#ifndef RELIEFNT_TEST_EXEC_SOURCE_FIXTURE_STUBS_H
#define RELIEFNT_TEST_EXEC_SOURCE_FIXTURE_STUBS_H
#ifndef RELIEFNT_TEST_REAL_EXEC_SOURCE
#include <assert.h>
#include <reliefnt/exec_file.h>
struct vfs_exec_source *vfs_exec_source_get(struct vfs_exec_source *source)
{assert(!source);return NULL;}
void vfs_exec_source_put(struct vfs_exec_source *source){assert(!source);}
int vfs_exec_source_read(const struct vfs_exec_source *source,uint64_t offset,
                            void *buffer,uint32_t capacity,uint32_t *read)
{(void)source;(void)offset;(void)buffer;(void)capacity;(void)read;assert(0);return -1;}
#endif
#endif
