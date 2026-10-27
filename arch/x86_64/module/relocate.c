/* SPDX-License-Identifier: Apache-2.0 */
#include "../../../kernel/module/internal.h"

/** @brief Return the number of bytes written by a supported RELA type.
 * @param type x86-64 relocation number.
 * @return Zero, four or eight; -ENOEXEC for unsupported GOT/TLS/other types.
 */
int module_relocation_width(unsigned int type)
{
    switch(type) {
    case 0: return 0;
    case 1: case 24: return 8;
    case 2: case 4: case 10: case 11: return 4;
    default: return -ENOEXEC;
    }
}
/** @brief Calculate a RELA in wide arithmetic before touching destination bytes.
 * @param target Writable section bytes; remains unchanged on error.
 * @param available Remaining bytes in that section.
 * @param type Supported x86-64 relocation selector.
 * @param symbol Absolute symbol address.
 * @param addend Signed explicit addend.
 * @param place Absolute address of the first relocation byte.
 * @return Zero or -ENOEXEC; exclusively owned loading mapping, no locks/callbacks.
 */
int module_apply_relocation(void *target,size_t available,unsigned int type,
                            uint64_t symbol,int64_t addend,uint64_t place)
{
    int width=module_relocation_width(type);
    if(width<0 || (size_t)width>available || (width && !target)) return -ENOEXEC;
    if(!width) return 0;
    __int128 value=(__int128)symbol+addend;
    if(type==2 || type==4 || type==24) {
        value-=(__int128)place;
        if(width==4 && (value<INT32_MIN || value>INT32_MAX)) return -ENOEXEC;
        if(width==8 && (value<INT64_MIN || value>INT64_MAX)) return -ENOEXEC;
    } else {
        if(value<0 || value>(__int128)UINT64_MAX) return -ENOEXEC;
        if(type==10 && value>UINT32_MAX) return -ENOEXEC;
        if(type==11 && (uint64_t)(int64_t)(int32_t)(uint64_t)value!=(uint64_t)value)
            return -ENOEXEC;
    }
    uint64_t bytes=(uint64_t)value;
    module_copy(target,&bytes,(size_t)width);
    return 0;
}
