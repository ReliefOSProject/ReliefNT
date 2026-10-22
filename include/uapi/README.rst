ReliefNT user/kernel boundary
============================

linux/ declares the native x86-64 Linux wire ABI; reliefos/ owns project
extensions. Neither i386 nor x32 is included. A syscall number declaration
does not implement a call. Export only the headers_install whitelist.

Private task state, pointers and helpers remain under include/reliefnt.
Public structures, ioctl values, sizes and flags must match the implemented
wire contract. Never patch musl constants to conceal a divergent kernel.
Declarations, host compilation and guest behavior are separate checks.

The musl runtime uses /lib/ld-musl-x86_64.so.1 and libreliefos.so.2.
libleonos.so.2 is a separately linked compatibility SONAME, not a symlink
substitute. Do not mix its FILE, errno, TLS or allocator objects with the
retired Picolibc runtime. Old private ABI binaries may need rebuilding.

UID/GID and Linux mode bits are independent of desktop roles. O_CREAT/mkdir
mode zero means no permissions. Accounts use passwd/shadow/group/gshadow and
PAM; populated retired private account stores are not silently converted.
See Documentation/userspace-api/abi.rst and descriptors.rst in this repository.
