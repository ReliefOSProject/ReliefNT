====
Swap
====

``CONFIG_SWAP`` is a core Boolean option and defaults to enabled.  When it is
disabled, ``swapon`` and ``swapoff`` return ``ENOSYS``; slot allocation
returns no slot and I/O reports ``ENOSYS``.

Enabling the kernel option does not create or activate an area.  An
administrator must first provide an owned, fully allocated backing on a
supported device, initialize it with ``mkswap``, and explicitly activate it
with ``swapon``.  ``mkswap`` overwrites the selected backing: verify its identity
and preserve any data before using it.  Check ``/proc/swaps`` after activation;
``swapoff`` attempts to restore the area's live pages before detaching it.
ReliefOS new installations may separately opt in to creating a swap file;
that installer option defaults to off.

Under memory pressure the reclaim path can swap eligible exclusive private
anonymous pages, including private COW copies.  Shared anonymous, SysV,
shared-file, device, locked, pinned, and cache-owned pages are not candidates.
Clean, proven reloadable file-cache pages may instead be discarded; there is
no general dirty shared-file writeback or swap of arbitrary shared backing.
When reclaim cannot make sufficient progress, OOM selection can publish a kill
signal to an eligible user address space; that publication does not itself
prove that all of its resources have already retired or that every allocation
will succeed.  These are implementation boundaries, not Linux/POSIX coverage
or a guarantee of a particular victim or response time.

The kernel accepts the Linux version 1 swap header on a regular native ext
file or an unmounted block partition/disk backed by an audited AHCI SATA disk.
The header must fit the backing, its bad-page entries must be distinct and in
range, and every file block through the declared last page must be allocated.
Holes and unsupported inode encodings are rejected.  Allocated unwritten ext
blocks are converted to zero-filled initialized blocks and synced before the
area is published.  The actual filesystem, inode, extent and block-device
identities stay pinned until swapoff.  Different non-overlapping files on the
same filesystem may be active together.  Priorities are honored, with
round-robin selection among equally preferred areas.

Activation requires ``CAP_SYS_ADMIN`` and normal read/write access to the
backing.  The active file retains its own writable inode lease, which excludes
executable deny-write leases.  A plain nontruncating writable open, including
one made before activation, is allowed.
Writes and truncation of the active inode are rejected, as are fallocate,
rename, unlink, and hard-link mutations.  Existing shared file mappings,
cached file pages, or already admitted writes cause activation to fail with
``EBUSY``; this conservative restriction avoids relying on unavailable
shared-file writeback.  New shared mappings, including read-only and
``PROT_NONE``, are rejected with ``ETXTBSY`` while active; private mappings
remain allowed.  A writable upgrade of an existing shared mapping is also
rejected.  A read-only remount is refused while a swap file on the
volume is active.  Raw writes to the containing filesystem partition, and
whole-disk writes to its disk, are excluded, but ordinary writes to unrelated
files continue.  Raw swap backing similarly excludes overlapping raw writes,
mounts and partition-table changes; raw reads are not excluded.  Existing open
file descriptions do not bypass these checks.

Only AHCI SATA currently advertises the required bounded synchronous storage
contract.  NVMe, USB, memory block providers and other unreviewed devices do
not advertise it and are not supported for swap activation.  A storage
operation already owned by another task is not stolen or retried indefinitely;
activation or slot I/O can return ``EBUSY``.  Fixed-block slot I/O allocates no
memory and does not enter filesystem allocation or journaling.  It uses the
provider's retained DMA buffers, holds execution/transport exclusion, and polls
synchronously with interrupts masked.  Before each attempted write it evicts
clean overlapping filesystem block-cache aliases on that device; dirty or
pinned cache entries remain owned by their transactions.  AHCI's finite
polling and recovery loops can still have substantial worst-case latency; this
is not an asynchronous or real-time latency guarantee.  A failed controller
STOP retains and quarantines driver-owned DMA until it can be stopped.  No
caller buffer or owned swap request remains pending on return.  A provider's unowned
``EAGAIN`` becomes a terminal error, while the public ``EAGAIN`` slot result
is reserved for a genuinely owned pollable request; this backend currently
queues none.

Discard flags are accepted but no discard/TRIM command is issued.  A valid
slot is never overwritten in place: an update must use a new slot so a failed
write cannot destroy the previous copy.  Swapoff disables new allocations in
the target area and restores referenced private anonymous pages in bounded
steps.  If draining fails, the area is made available again and its remaining
slot references and backing stay owned.  The original error is retained while
non-OOM reclaim makes a best-effort attempt to restore ordinary free frames,
limited by pages actually restored during that drain.  Reclaim runs in small
transactions with identity checks between them; it stops at the free-frame
goal, a completed nonproductive scan, foreign storage contention, cancellation,
another error, or a finite topology-derived work limit.  Concurrent allocations,
shared address spaces and topology churn mean the goal is not guaranteed, and
the total syscall latency can be substantial despite the bounded individual
transactions.  Incomplete recovery is reported in the kernel log.  These
The best-effort recovery is not a guarantee that the failed operation, or a
subsequent allocation, will succeed.

``/proc/swaps`` captures each published area, including one being drained,
with the current retained path, type, usable and used KiB, and signed priority.
It escapes spaces, tabs, newlines and backslashes in paths.  A retained path
follows ancestor renames, and each open file provides a stable snapshot that
can be read in fragments.  During drain, system swap totals include only the
area's still-used slots; the area row retains its original usable capacity.

``/proc/meminfo`` reports RAM and cache in KiB, and ``sysinfo`` uses the same
snapshot with ``mem_unit`` 1024.  Available RAM excludes swap, the emergency
reserve and cache pages that are not proven reclaimable.  Dirty counts only
confirmed writes to resident shared file-cache pages; a writable mapping alone
is unknown, not proof of a write.  Hardware dirty observations are sampled
from live mappings and retained when mappings disappear; there is no general
file-cache writeback protocol.  ``SwapCached`` is zero because the kernel has
no swap cache.

``/proc/vmstat`` counts actual logical user faults, completed swap transfers,
direct/background candidate scans and released frames, and committed OOM kill
signal publications.  ``pgmajfault`` counts a user fault's first completed
swap read or file-provider page read; a lower filesystem or block cache may
serve that provider read, so it is not a physical-device I/O count.  Multiple
deferred retries of one fault count once, while
kernel copy/prefault helpers do not create user-fault events.  The OOM kernel
diagnostic identifies the PID at signal publication; process retirement and
actual released memory are subsequent events.
