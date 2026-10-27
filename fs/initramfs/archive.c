/* SPDX-License-Identifier: Apache-2.0 */
/* Wire format: Linux driver-api/early-userspace/buffer-format (newc/crc).
 * c_check is the specified unsigned byte sum, not a CRC polynomial. */
#include "archive.h"
#include <linux/errno.h>
#include <linux/stat.h>

/** @brief Compare exactly a known wire-field extent without reading its terminator.
 * @param left Readable length bytes. @param right Readable length bytes.
 * @param length Exact compared bytes. @return True for equality. No locks,
 * allocation, callback or pointer retention; both operands remain caller-owned.
 */
static bool archive_equal(const uint8_t *left,const char *right,size_t length)
{
    for(size_t i=0;i<length;++i)if(left[i]!=(uint8_t)right[i])return false;
    return true;
}

/** @brief Decode one complete eight-byte unsigned ASCII hexadecimal field.
 * @param text Readable eight-byte wire value, no NUL required.
 * @param out Required scalar output, valid only when true is returned.
 * @return False for any nonhex byte; no partial output, arithmetic overflow,
 * allocation, callback or ownership transfer. Upper/lowercase are equivalent.
 */
static bool archive_hex(const uint8_t *text,uint32_t *out)
{
    uint32_t value=0;
    for(unsigned i=0;i<8;++i) {
        uint8_t digit=text[i];
        if(digit>='0' && digit<='9')digit-='0';
        else if(digit>='a' && digit<='f')digit=digit-'a'+10;
        else if(digit>='A' && digit<='F')digit=digit-'A'+10;
        else return false;
        value=(value<<4)|digit;
    }
    *out=value;return true;
}

/** @brief Recognize only standard CPIO/Linux inode types and their data rules.
 * @param mode Full Linux inode type and permission word.
 * @param size Exact wire data bytes. @return True for a supported valid type;
 * symlinks require data and nonregular/nonsymlink nodes require no data.
 * Pure metadata validation, no path resolution, allocation or callback.
 */
static bool archive_type_valid(uint32_t mode,uint32_t size)
{
    uint32_t type=mode&LINUX_S_IFMT;
    if(type==LINUX_S_IFREG)return true;
    if(type==LINUX_S_IFLNK)return size!=0;
    return !size && (type==LINUX_S_IFDIR || type==LINUX_S_IFCHR ||
        type==LINUX_S_IFBLK || type==LINUX_S_IFIFO || type==LINUX_S_IFSOCK);
}

/** @brief Walk bounded newc/crc records and optional exact archive-end markers.
 * @param image Stable length-byte input, nullable only for empty input.
 * @param length Complete source length. @param visitor Optional pinned callback;
 * NULL entry signals trailer, nonNULL fields borrow exact source bytes.
 * @param context Borrowed caller callback state.
 * @return Zero or input/format/checksum/unsupported/callback errno. No archive
 * writes, allocation, lock or I/O. A callback error terminates immediately;
 * callers own rollback, source lifetime and any hard-link tuple allocations.
 */
int initramfs_scan(const void *image,size_t length,
                      int (*visitor)(void *,const struct initramfs_entry *),void *context)
{
    if(!image && length)return -EINVAL;
    const uint8_t *bytes=image;size_t position=0;
    while(position<length) {
        while(position<length && !bytes[position])++position;
        if(position==length)return 0;
        size_t header=position,remaining=length-header;
        if(remaining>=2 && bytes[header]==0x1f && bytes[header+1]==0x8b)return -EOPNOTSUPP;
        if(remaining<110)return -ENOEXEC;
        bool crc=archive_equal(bytes+header,"070702",6);
        if(!crc && !archive_equal(bytes+header,"070701",6))return -ENOEXEC;
        uint32_t field[13];
        for(unsigned i=0;i<13;++i)if(!archive_hex(bytes+header+6+i*8,&field[i]))return -ENOEXEC;
        uint32_t names=field[11],size=field[6];
        if(!names || names>remaining-110)return -ENOEXEC;
        const uint8_t *name=bytes+header+110;
        if(name[names-1])return -ENOEXEC;
        for(uint32_t i=0;i+1<names;++i)if(!name[i])return -ENOEXEC;
        size_t name_end=110u+(size_t)names;
        if(name_end>SIZE_MAX-3)return -ENOEXEC;
        size_t data_relative=(name_end+3)&~(size_t)3;
        if(data_relative>remaining || size>remaining-data_relative)return -ENOEXEC;
        const uint8_t *data=bytes+header+data_relative;
        bool trailer=names==11 && archive_equal(name,"TRAILER!!!",10);
        if(trailer ? size!=0 : !archive_type_valid(field[1],size))return -ENOEXEC;
        if(!crc && field[12])return -ENOEXEC;
        if(crc) {
            uint32_t checksum=0;
            for(uint32_t i=0;i<size;++i)checksum+=data[i];
            if(checksum!=field[12])return -EBADMSG;
        }
        struct initramfs_entry entry={.name=(const char *)name,.name_length=names-1,.data=data,.size=size,
            .inode=field[0],.mode=field[1],.uid=field[2],.gid=field[3],.links=field[4],.mtime=field[5],
            .device_major=field[7],.device_minor=field[8],.rdevice_major=field[9],.rdevice_minor=field[10]};
        if(visitor) {
            int rc=visitor(context,trailer ? NULL : &entry);
            if(rc)return rc<0 ? rc : -EINVAL;
        }
        size_t end_relative=data_relative+(size_t)size;
        position=header+end_relative;
        size_t padding=(0u-end_relative)&3u;
        /* EOF may terminate an archive without a trailer. Any following record
         * still requires its complete zero alignment before a new header. */
        while(padding && position<length) {
            if(bytes[position])return -ENOEXEC;
            ++position;--padding;
        }
    }
    return 0;
}
