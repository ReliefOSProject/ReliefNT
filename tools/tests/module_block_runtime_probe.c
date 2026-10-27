/* SPDX-License-Identifier: Apache-2.0 */
/* Selected PID1 only; an owned, marker-bearing extra disk is mandatory. */
#define main module_base_probe
#include "module_runtime_probe.c"
#undef main
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/uio.h>
#ifndef MODULE_BLOCK_QA_DEVICE
#define MODULE_BLOCK_QA_DEVICE "/dev/sdb1"
#endif
static const char marker[]="ReliefNT K6a private block mount QA\n";
static unsigned char payload[131072],observed[131072];
/** @brief Verify all six vector operations on the marker-owned ext4 payload.
 * @param file Borrowed writable descriptor of the complete known payload.
 * @return None; all writes reproduce existing bytes, preserving its final SHA.
 * Explicit-position operations retain the shared offset; native x86_64 uses a
 * full64 pos_l and ignores pos_h. No host invocation can reach this function.
 */
static void block_vector_probe(int file)
{
 struct iovec output[]={{payload,11},{payload+11,32759},{payload+32770,sizeof(payload)-32770}};
 struct iovec input[]={{observed,17},{observed+17,32751},{observed+32768,sizeof(observed)-32768}};
 require(!lseek(file,0,SEEK_SET) && writev(file,output,3)==sizeof(payload) &&
  lseek(file,0,SEEK_CUR)==sizeof(payload),"native writev full multi-block payload and shared position");
 memset(observed,0,sizeof(observed));
 require(!lseek(file,0,SEEK_SET) && readv(file,input,3)==sizeof(observed) &&
  !memcmp(payload,observed,sizeof(payload)),"native readv full multi-block payload");
 const long operations[]={SYS_preadv,SYS_pwritev,SYS_preadv2,SYS_pwritev2};
 const uint64_t ignored=UINT64_C(0xfedcba9876543210);
 for(unsigned i=0;i<sizeof(operations)/sizeof(operations[0]);++i) {
  struct iovec from[]={{payload+7,4093},{payload+4100,1011}},to[]={{observed,3001},{observed+3001,2103}};
  memset(observed,0,5104);require(lseek(file,31,SEEK_SET)==31,"reserve vector shared-position sentinel");
  require(syscall(operations[i],file,i&1 ? from : to,2,UINT64_C(7),ignored,UINT64_C(0x100000000))==5104 &&
   lseek(file,0,SEEK_CUR)==31,"native positional vector ABI preserves shared position");
  if(!(i&1))require(!memcmp(observed,payload+7,5104),"native positional vector read bytes");
 }
 struct iovec tail[]={{observed,3},{observed+3,5}};memset(observed,0xcc,8);
 require(syscall(SYS_preadv,file,tail,2,(uint64_t)(sizeof(payload)-4),ignored)==4 &&
  !memcmp(observed,payload+sizeof(payload)-4,4) && observed[4]==0xcc && lseek(file,0,SEEK_CUR)==31,
  "native preadv stops at EOF within second vector without touching suffix");
 require(syscall(SYS_preadv2,file,tail,2,UINT64_C(0xffffffff),0,0)==0 && lseek(file,0,SEEK_CUR)==31,
  "native v2 UINT32_MAX is an explicit positive position");
 errno=0;require(syscall(SYS_preadv,file,tail,2,UINT64_MAX,ignored)==-1 && errno==EINVAL,
  "native old positional vector rejects negative full64 position");
 struct iovec from[]={{payload+7,3},{payload+10,5}};
 require(lseek(file,7,SEEK_SET)==7 && syscall(SYS_pwritev2,file,from,2,UINT64_MAX,ignored,0)==8 &&
  lseek(file,0,SEEK_CUR)==15,"native pwritev2 minus-one selects shared position");
 memset(observed,0,8);
 require(lseek(file,7,SEEK_SET)==7 && syscall(SYS_preadv2,file,tail,2,UINT64_MAX,ignored,0)==8 &&
  !memcmp(observed,payload+7,8) && lseek(file,0,SEEK_CUR)==15,"native preadv2 minus-one selects shared position");
 puts("MODULE_RUNTIME_VECTOR all6 operations, native ABI, full payload, EOF and shared/positional offsets");
}
/** @brief Verify owned test media before any payload mutation.
 * @param directory Borrowed descriptor of the freshly mounted filesystem root.
 * @return None; the probe stops on absent or mismatched ownership marker.
 */
static void check_owned_marker(int directory)
{
 int fd=openat(directory,"reliefnt-ko-owned",O_RDONLY);char bytes[64]={0};
 require(fd>=0 && read(fd,bytes,sizeof(bytes))==(ssize_t)(sizeof(marker)-1) &&
  !memcmp(bytes,marker,sizeof(marker)-1) && !close(fd),"verify private block media marker before payload mutation");
}
/** @brief Verify all Linux timestamp layouts on the actual mounted inode.
 * @param directory Borrowed actual parent directory descriptor.
 * @param file Borrowed writable regular-file descriptor.
 * @param path Actual absolute pathname of this private probe payload.
 * @return None. Leaves the final inode mtime at 888 for persistence checking.
 */
static void block_timestamp_probe(int directory,int file,const char *path)
{
 struct stat status;int64_t seconds[2]={111,222};
 require(!syscall(SYS_utime,path,seconds) && !fstat(file,&status) && status.st_atime==111 && status.st_mtime==222,
  "native utime layout updates actual disk inode");
 struct timeval values[2]={{333,123456},{444,654321}};
 require(!syscall(SYS_utimes,path,values) && !fstat(file,&status) && status.st_atime==333 && status.st_mtime==444,
  "native utimes timeval layout");
 values[0].tv_sec=555;values[1].tv_sec=666;
 require(!syscall(SYS_futimesat,directory,"qa-payload",values) && !fstat(file,&status) && status.st_atime==555 && status.st_mtime==666,
  "native futimesat relative parent layout");
 struct timespec times[2]={{777,123456},{888,654321}};
 require(!syscall(SYS_utimensat,file,NULL,times,0) && !fstat(file,&status) && status.st_atime==777 && status.st_mtime==888,
  "native futimens timespec layout on retained FD");
}

/** @brief Attach the owned physical filesystem twice with independent policy.
 * @param device Standard node for the marker-owned extra test partition.
 * @param target Precreated private alias mountpoint.
 * @param original Borrowed original writable payload descriptor.
 * @param directory Required owned alias root descriptor output.
 * @return Owned readonly alias payload FD. Changes a small range through each
 * alias and restores known bytes before returning. Both descriptions must see
 * coherent inode/data, and statfs must report their own mount restrictions.
 * This guest-only helper never selects a host device or unmarked filesystem.
 */
static int block_alias_open(const char *device,const char *target,int original,int *directory)
{
 require(!mount(device,target,"ext4",MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME,NULL),
  "attach same actual block superblock with independent mount policy");
 *directory=open(target,O_RDONLY|O_DIRECTORY);require(*directory>=0,"open second actual block attachment");
 check_owned_marker(*directory);
 int file=openat(*directory,"qa-payload",O_RDWR);struct stat one,two;
 require(file>=0 && !fstat(original,&one) && !fstat(file,&two) && one.st_dev==two.st_dev && one.st_ino==two.st_ino,
  "same physical attachment aliases share actual inode identity");
 unsigned char changed[16];for(unsigned i=0;i<sizeof(changed);++i)changed[i]=payload[4093+i]^0xa5;
 require(pwrite(file,changed,sizeof(changed),4093)==sizeof(changed) &&
  pread(original,observed,sizeof(changed),4093)==sizeof(changed) && !memcmp(observed,changed,sizeof(changed)),
  "second block attachment writes are immediately coherent in original OFD");
 require(pwrite(original,payload+4093,sizeof(changed),4093)==sizeof(changed) &&
  pread(file,observed,sizeof(changed),4093)==sizeof(changed) && !memcmp(observed,payload+4093,sizeof(changed)),
  "original attachment restores bytes coherently through second OFD");
 struct statfs a,b;const unsigned long selected=MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME|4096;
 require(!fstatfs(original,&a) && !fstatfs(file,&b) && (a.f_flags&selected)==4096 &&
  (b.f_flags&selected)==(MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME),"native statfs attachment policy does not cross aliases");
 require(!close(file),"release alias writer before shared readonly transition");
 file=openat(*directory,"qa-payload",O_RDONLY);require(file>=0,"retain readonly alias payload");return file;
}

/** @brief Check native disk access-time policy using two actual mount aliases.
 * @param directory Borrowed original filesystem-root descriptor.
 * @param file Borrowed original writable regular-file descriptor.
 * @param alias_directory Borrowed alias root descriptor.
 * @param alias_file Borrowed readonly description of the same regular inode.
 * @param alias Required absolute alias mountpoint on the marker-owned test disk.
 * @return None. Restores alias noatime policy; the caller sets final payload
 * timestamps afterward. Executes only after the ReliefNT/PID1 and disk marker
 * guards. Every mutation stays inside this private test filesystem.
 */
static void block_access_probe(int directory,int file,int alias_directory,int alias_file,const char *alias)
{
 struct timespec before,after;
 require(!clock_gettime(CLOCK_REALTIME,&before),"read wall clock for disk atime probe");
 struct timespec values[2]={{before.tv_sec+7200,0},{1,0}};
 require(!futimens(file,values),"set future access time on owned disk inode");
 unsigned char byte;struct stat status;
 require(pread(alias_file,&byte,1,0)==1 && !fstat(file,&status) && status.st_atime==values[0].tv_sec,
  "noatime alias preserves shared inode access time");
 require(pread(file,&byte,1,0)==1 && !fstat(file,&status) && status.st_atime==values[0].tv_sec,
  "relatime preserves access time newer than mtime and ctime");
 require(!mount(NULL,alias,NULL,MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_STRICTATIME,NULL),
  "set strict access time on one real disk alias");
 require(pread(alias_file,&byte,1,0)==1 && !fstat(file,&status) &&
  !clock_gettime(CLOCK_REALTIME,&after) && status.st_atime>=before.tv_sec && status.st_atime<=after.tv_sec,
  "strictatime alias updates the actual shared disk inode");
 values[0].tv_sec=before.tv_sec-86401;
 require(!futimens(file,values) && pread(file,&byte,1,0)==1 && !fstat(file,&status) &&
  !clock_gettime(CLOCK_REALTIME,&after) && status.st_atime>=before.tv_sec && status.st_atime<=after.tv_sec,
  "relatime refreshes disk access time older than one day");
 require(!mount(NULL,alias,NULL,MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_RELATIME|MS_NODIRATIME,NULL),
  "set directory-only access suppression on alias");
 values[0].tv_sec=1;values[1].tv_sec=2;
 require(!futimens(directory,values),"set old access time on owned disk directory");
 unsigned char entries[512];
 require(syscall(SYS_getdents64,alias_directory,entries,sizeof(entries))>0 &&
  !fstat(directory,&status) && status.st_atime==1,"nodiratime alias preserves directory access time");
 require(syscall(SYS_getdents64,directory,entries,sizeof(entries))>0 &&
  !fstat(alias_directory,&status) && !clock_gettime(CLOCK_REALTIME,&after) &&
  status.st_atime>=before.tv_sec && status.st_atime<=after.tv_sec,
  "original disk directory retains its independent access-time policy");
 require(!mount(NULL,alias,NULL,MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME,NULL),
  "restore original alias noatime policy");
 puts("MODULE_RUNTIME_DISK_ATIME actual shared inode, noatime/relatime/strictatime and nodiratime aliases");
}

/** @brief Exercise one actual GPT/ext4 factory and retire all owned references.
 * @return None. Uses the fixed extra test disk only after checking its marker;
 * private mountpoints, data writes and provider lifetime are validated together.
 */
static void block_runtime_probe(void)
{
 const char *target="/dev/shm/ko-block-mount",*moved="/dev/shm/ko-block-moved",*alias="/dev/shm/ko-block-alias",*device=MODULE_BLOCK_QA_DEVICE;
 require(!mkdir(target,0700) && !mkdir(moved,0700) && !mkdir(alias,0700),"reserve private block targets");
 require(!mount(device,target,"ext4",MS_RELATIME,NULL),"mount actual detached ext4 block factory");
 int directory=open(target,O_RDONLY|O_DIRECTORY);require(directory>=0,"open new block mount root");check_owned_marker(directory);
 struct statfs fs;require(!fstatfs(directory,&fs) && fs.f_type==0xef53,"real ext4 filesystem identity");
 int file=openat(directory,"qa-payload",O_CREAT|O_EXCL|O_RDWR,0600);require(file>=0,"create private external filesystem payload");
 for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)(i*13u+i/257u);
 size_t done=0;while(done<sizeof(payload)){ssize_t n=write(file,payload+done,sizeof(payload)-done);require(n>0,"write complete block payload");done+=(size_t)n;}
 block_vector_probe(file);
 int alias_directory=-1,alias_file=block_alias_open(device,alias,file,&alias_directory);
 block_access_probe(directory,file,alias_directory,alias_file,alias);
 char full_path[128];snprintf(full_path,sizeof(full_path),"%s/qa-payload",target);
 block_timestamp_probe(directory,file,full_path);
 require(!fdatasync(file) && !fsync(directory),"sync actual block file and root metadata");
 require(!mount(target,moved,NULL,MS_MOVE,NULL),"move live disk mount with held OFDs");
 require(pread(file,observed,sizeof(observed),0)==sizeof(observed) && !memcmp(payload,observed,sizeof(payload)),"held disk OFD preserves whole payload after mount move");
 unsigned char *mapping=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,file,0);
 require(mapping!=MAP_FAILED && !memcmp(mapping,payload,4096) && !close(file) && !close(directory),"disk VMA retains actual provider after descriptor close");
 errno=0;require(umount(moved)==-1 && errno==EBUSY,"mapped disk inode keeps mounted backend busy");
 require(!munmap(mapping,4096),"release final mapped block OFD");
 require(!mount((const char *)(uintptr_t)1,moved,(const char *)(uintptr_t)1,MS_REMOUNT|MS_RDONLY|MS_RELATIME,NULL),"readonly remount actual disk context");
 directory=open(moved,O_RDONLY|O_DIRECTORY);require(directory>=0,"open readonly moved block root");check_owned_marker(directory);
 errno=0;require(openat(directory,"qa-payload",O_WRONLY)==-1 && errno==EROFS,"real block readonly policy rejects writes");
 require(!close(directory) && !umount(moved),"flush and retire actual disk backend");
 struct statfs alias_state;
 require(!fstatfs(alias_file,&alias_state) && (alias_state.f_flags&MS_RDONLY) &&
  pread(alias_file,observed,sizeof(observed),0)==sizeof(observed) && !memcmp(payload,observed,sizeof(payload)),
  "nonfinal block detach preserves alias data and shared readonly state");
 require(!close(alias_file) && !close(alias_directory) && !umount(alias),"final block alias closes and retires shared backend");
 require(!mount(device,target,"ext4",MS_RELATIME,NULL),"recreate actual disk backend after slot reclaim");
 directory=open(target,O_RDONLY|O_DIRECTORY);require(directory>=0,"open recreated disk mount");check_owned_marker(directory);
 file=openat(directory,"qa-payload",O_RDONLY);struct stat persisted;
 require(file>=0 && !fstat(file,&persisted) && persisted.st_mtime==888,"new mount preserves committed inode timestamp");
 require(read(file,observed,sizeof(observed))==sizeof(observed) &&
  !memcmp(payload,observed,sizeof(payload)) && !close(file),"new mount reads persisted full block payload");
 require(!close(directory) && !umount(target),"final actual filesystem flush/detach");
 require(!rmdir(target) && !rmdir(moved) && !rmdir(alias),"remove private block targets");
 puts("MODULE_RUNTIME_BLOCK_ALIASES coherent inode/data, independent mount policy, shared readonly and last-detach retirement");
 puts("MODULE_RUNTIME_BLOCK actual GPT/ext4 mount, full data, move, mmap busy, readonly, flush and remount persistence");
}
/** @brief Verify the prepared boot namespace before block/module mutations.
 * @return None; stops on incorrect type, owner or volatile-directory modes.
 * An optional private carrier marker enables stale-tree and symlink cleanup
 * checks; the marker is read only after the main ReliefNT/PID1 host guard.
 */
static void boot_runtime_probe(void)
{
 const char *directories[]={"/run","/run/lock","/dev/shm"};
 const mode_t modes[]={0755,0755,01777};struct stat status;
 for(unsigned i=0;i<sizeof(directories)/sizeof(directories[0]);++i)
  require(!lstat(directories[i],&status) && S_ISDIR(status.st_mode) &&
   (status.st_mode&07777)==modes[i] && !status.st_uid && !status.st_gid,
   "boot runtime actual directory type, permissions and root ownership");
 errno=0;int owned=open("/ko-runtime-owned",O_RDONLY|O_NOFOLLOW);
 if(owned<0){require(errno==ENOENT,"optional boot carrier marker lookup");return;}
 const char expected[]="ReliefNT owned boot runtime QA\n";char bytes[sizeof(expected)]={0};
 require(read(owned,bytes,sizeof(bytes))==sizeof(expected)-1 &&
  !memcmp(bytes,expected,sizeof(expected)-1) && !close(owned),"boot cleanup preserves external marker through stale symlink");
 errno=0;require(lstat("/boot/reliefos/state/kerneldebug.next",&status)<0 && errno==ENOENT,
  "boot consumes the owned one-shot ESP control before PID1");
 const char *removed[]={"/run/ko-stale","/run/ko-stale-link"};
 for(unsigned i=0;i<sizeof(removed)/sizeof(removed[0]);++i){errno=0;
  require(lstat(removed[i],&status)<0 && errno==ENOENT,"boot removes actual stale tree and literal symlink");}
 puts("MODULE_RUNTIME_BOOT_OBJECTS actual root/run/shm, corrected modes, stale tree/literal symlink cleanup and consumed ESP marker");
}
/** @brief Exercise the inherited implicit TTY metadata without pathname replay.
 * @return None; halts PID1 on incorrect type, ownership, mode or restoration.
 * Called only after the ReliefNT/PID1 guard. Descriptor zero stays open; all
 * metadata is restored before return. No provider/module lock spans syscalls.
 */
static void boot_virtual_permissions_probe(void)
{
 struct stat original,changed;
 require(!fstat(0,&original) && S_ISCHR(original.st_mode),"inherited implicit TTY metadata");
 require(!fchmod(0,0660) && !fchown(0,123,456) && !fstat(0,&changed) &&
  (changed.st_mode&07777)==0660 && changed.st_uid==123 && changed.st_gid==456,
  "implicit TTY chmod/chown uses its admitted virtual inode");
 require(!fchown(0,original.st_uid,original.st_gid) && !fchmod(0,original.st_mode&07777) &&
  !fstat(0,&changed) && (changed.st_mode&07777)==(original.st_mode&07777) &&
  changed.st_uid==original.st_uid && changed.st_gid==original.st_gid,
  "restore inherited TTY metadata");
 puts("MODULE_RUNTIME_VIRTUAL_PERMISSIONS implicit TTY exact metadata and restoration");
}
/** @brief Verify an optional owned optical carrier through the boot namespace.
 * @return None; halts PID1 on wrong contents, type, policy or write admission.
 * The root marker opts into this check only after the ReliefNT/PID1 guard.
 * Every open descriptor closes; no module/graph lock is held over syscalls.
 */
static void boot_optical_probe(void)
{
 errno=0;int carrier=open("/ko-optical-required",O_RDONLY|O_NOFOLLOW);
 if(carrier<0){require(errno==ENOENT,"optional optical carrier marker lookup");return;}
 const char expected[]="ReliefNT owned optical QA\n";char bytes[sizeof(expected)]={0};
 require(read(carrier,bytes,sizeof(bytes))==sizeof(expected)-1 &&
  !memcmp(bytes,expected,sizeof(expected)-1) && !close(carrier),"owned root optical marker");
 struct statfs fs;struct stat status;
 require(!statfs("/media/cdrom0",&fs) && fs.f_type==0x9660 && (fs.f_flags&ST_RDONLY),
  "boot optical actual ISO9660 readonly attachment");
 require(!stat("/media/cdrom0",&status) && S_ISDIR(status.st_mode),"optical mount is a directory");
 int optical=open("/media/cdrom0/koopt.qa",O_RDONLY|O_NOFOLLOW);
 memset(bytes,0,sizeof(bytes));
 require(optical>=0 && read(optical,bytes,sizeof(bytes))==sizeof(expected)-1 &&
  !memcmp(bytes,expected,sizeof(expected)-1) && !close(optical),"boot optical exact independent ISO payload");
 errno=0;require(open("/media/cdrom0/koopt.qa",O_WRONLY)==-1 && errno==EROFS,
  "readonly optical writer rejected before mutation");
 puts("MODULE_RUNTIME_OPTICAL explicit numbered ISO attachment, exact marker and readonly writer rejection");
}
/** @brief Verify actual boot-ESP FAT timestamps only on an explicitly owned carrier.
 * @return None; skips an absent opt-in marker and halts on a wrong marker/policy.
 * After the main ReliefNT/PID1 guard, requires the exact private FAT marker
 * before any mutation. Uses new exclusive names, restores reported boot-mount
 * policy and removes both linked/orphan test objects. Normalized FAT atime is
 * a UTC date; native rename/held-unlink I/O reaches the actual block transport.
 */
static void boot_fat_access_probe(void)
{
 const char expected[]="ReliefNT owned FAT timestamp QA\n";char guard[sizeof(expected)]={0};
 errno=0;int marker_fd=open("/ko-fat-required",O_RDONLY|O_NOFOLLOW);
 if(marker_fd<0){require(errno==ENOENT,"optional owned FAT carrier marker");return;}
 require(read(marker_fd,guard,sizeof(guard))==sizeof(expected)-1 &&
  !memcmp(guard,expected,sizeof(expected)-1) && !close(marker_fd),"owned FAT carrier content before mutations");
 struct statfs original;
 require(!statfs("/boot",&original) && original.f_type==0x4d44 && !(original.f_flags&ST_RDONLY),"owned writable boot ESP uses actual FAT");
 const unsigned long policy=MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_NOATIME|MS_NODIRATIME;
 unsigned long restore=(unsigned long)original.f_flags&policy;
 if(original.f_flags&4096)restore|=MS_RELATIME; /* Linux ST_RELATIME differs from MS_RELATIME. */
 else if(!(restore&MS_NOATIME))restore|=MS_STRICTATIME;
 int directory=open("/boot/reliefos",O_RDONLY|O_DIRECTORY);require(directory>=0,"open owned ESP test parent");
 const char *name="ko-fat-time",*renamed="ko-fat-renamed";
 int old=openat(directory,name,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);
 require(old>=0 && write(old,"fat",3)==3,"create owned FAT timestamp payload");
 struct stat before,after;char bytes[3];
 require(!fstat(old,&before) && !mount(NULL,"/boot",NULL,MS_REMOUNT|MS_NOATIME,NULL),"set boot FAT noatime policy");
 require(pread(old,bytes,sizeof(bytes),0)==sizeof(bytes) && !memcmp(bytes,"fat",3) &&
  !fstat(old,&after) && after.st_atime==before.st_atime,"FAT noatime preserves inode metadata");
 struct timespec first,last;require(!clock_gettime(CLOCK_REALTIME,&first),"FAT access clock lower bound");
 require(!mount(NULL,"/boot",NULL,MS_REMOUNT|MS_STRICTATIME,NULL) && pread(old,bytes,sizeof(bytes),0)==sizeof(bytes) &&
  !fstat(old,&after) && !clock_gettime(CLOCK_REALTIME,&last) &&
  after.st_atime>=first.tv_sec/86400*86400 && after.st_atime<=last.tv_sec/86400*86400 && !after.st_atim.tv_nsec,
  "native FAT strict access persists date-resolution metadata");
 require(!fsync(old) && !renameat(directory,name,directory,renamed) && !fstat(old,&before),"native FAT rename preserves held timestamp inode");
 require(!unlinkat(directory,renamed,0),"unlink owned FAT inode while its description remains open");
 int replacement=openat(directory,renamed,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);
 require(replacement>=0 && write(replacement,"new",3)==3 && !fstat(replacement,&before),"reuse FAT directory name with independent inode");
 require(pread(old,bytes,sizeof(bytes),0)==sizeof(bytes) && !memcmp(bytes,"fat",3) &&
  !fstat(replacement,&after) && before.st_atime==after.st_atime && before.st_atim.tv_nsec==after.st_atim.tv_nsec,
  "orphan FAT access never updates the reused directory record");
 require(!close(old) && !close(replacement) && !unlinkat(directory,renamed,0) && !fsync(directory) && !close(directory),
  "retire owned FAT objects and flush actual metadata");
 require(!mount(NULL,"/boot",NULL,MS_REMOUNT|restore,NULL),"restore original boot FAT attachment policy");
 puts("MODULE_RUNTIME_FAT_ATIME actual ESP, date precision, noatime/strictatime, rename and orphan-slot isolation");
}
/** @brief Run the block/module test only as ReliefNT PID1.
 * @return Host refusal77 before mutators, otherwise the base module status.
 */
#ifndef MODULE_BLOCK_PROBE_ENTRY
#define MODULE_BLOCK_PROBE_ENTRY main
#endif
int MODULE_BLOCK_PROBE_ENTRY(void)
{
 struct utsname identity;if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1){fputs("Guest-only block probe: refusing host mutations\n",stderr);return 77;}
 int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
 setvbuf(stdout,NULL,_IONBF,0);puts("MODULE_RUNTIME_BLOCK_READY");boot_runtime_probe();boot_virtual_permissions_probe();boot_optical_probe();boot_fat_access_probe();block_runtime_probe();return module_base_probe();
}
