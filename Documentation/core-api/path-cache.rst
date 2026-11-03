=================
VFS name caching
=================

The VFS caches one-component lookup results before entering the native
filesystem.  Successful metadata and ``ENOENT`` results are cached; permission,
I/O, allocation, retry and stale-handle errors are never negative entries.
This is an internal optimization.  Linux syscall numbers, errno values,
pathname resolution and POSIX permission checks do not change.

Coherency and ownership
=======================

A provider opts in with ``vfs_backend_ops.lookup_generation``.  A nonzero
64-bit token identifies a period in which names and the metadata returned by
``lookup`` remain valid.  Every namespace or lookup-metadata mutation must
advance the token, including partial committed errors and direct provider
users.  A zero token or an absent callback disables caching.  A lookup is
inserted only if the token stayed unchanged through lookup and inode retain.

Native codecs must explicitly opt in with ``STORAGE_FS_LOOKUP_EPOCH``;
unmarked module-provided codecs remain uncached.  Audited disk filesystems
combine the conservative storage metadata epoch with their exact block
device's write epoch.  Direct ``block_device_write`` calls invalidate native
metadata caches before transport entry; invalidation also covers partial or
failed writes.  A revoked device disables name-cache hits so its ordinary
filesystem error path remains observable.  Tmpfs has an instance-local epoch updated by
its own mutation paths, including writes, truncation, direct namespace calls
and symlink rewrites.  Procfs, sysfs and devfs do not opt in: process exit,
module changes and device hotplug remain immediately visible.

Keys contain the owning mount, parent inode identity and literal name.
The provider's ``same_node`` comparison resolves hash collisions.  Names do
not become global path strings, so mount traversal, dot-dot, symlinks and
held directory descriptors retain their existing resolver semantics.

Entries contain copied metadata only.  They retain no inode, dentry, path,
mount or module reference.  A positive hit still binds and retains the actual
inode before returning a path.  Stat and permissions remain provider queries,
and the walker checks directory search permissions on every traversal.
Unmount and unlink are not delayed by cache ownership.  Each mount owns a
weak entry list, purged before its final root/backend/module release.
If a provider has recycled an unused inode-key cache record, a failed cached
retain with ``ENOENT`` or ``ESTALE`` purges the mount's name snapshots and
retries its normal directory lookup.  A stale identity does not become a
negative name entry.

Budget and reclaim
==================

A hash index serves lookup; a global LRU queue controls eviction.  Cache
allocation payloads have a shared 512 KiB budget, plus the fixed hash table
and normal heap allocator overhead.  This budget bounds optional unused
metadata, not the number of accessible files or live inode handles.

Insertion failure leaves the completed lookup result unchanged.  Eviction
frees one allocation without callbacks, inode drops, allocation or I/O.
Memory-pressure reclaim evicts at most eight names per batch before scanning
file pages, even when more names remain; each eviction consumes the existing
batch work budget.  A physical-release limit also caps the name count: every
name is one small slab allocation and can release at most one backing page.  An
evicted entry is not counted as a freed physical page unless the allocator's
free-page count actually increases.

Validation
==========

``tools/test_vfs_lookup_cache.py`` links the production VFS/cache with a
counted provider.  It checks positive/negative hits, generations, distinct
parents, dynamic-provider bypass, uncached errors, fresh search permissions,
component bounds, cache allocation failure and budgeted reclaim.
``test_mount_tree.py``, ``test_mount_backend.py`` and ``test_inotify_vfs.py``
exercise retained objects, native filesystem mutations and notifications.
OS image staging and QEMU orchestration belong to the ReliefOS repository;
native Linux-ABI runtime probes remain in this repository.
