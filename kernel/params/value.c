/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct parameter_string {
    struct parameter_string *next;
    const struct module_parameter *parameter;
    char bytes[];
};

/** @brief Return the scalar destination width without dereferencing it.
 * @param parameter Stable borrowed descriptor.
 * @return Byte width or zero for an unsupported kind; no locks or allocation.
 */
size_t parameter_value_width(const struct module_parameter *parameter)
{
    switch(parameter->type) {
    case MODULE_PARAMETER_bool:return sizeof(bool);
    case MODULE_PARAMETER_int:case MODULE_PARAMETER_uint:return sizeof(int);
    case MODULE_PARAMETER_long:case MODULE_PARAMETER_ulong:return sizeof(long);
    case MODULE_PARAMETER_charp:return sizeof(char *);
    default:return 0;
    }
}
/** @brief Compare names using Linux dash/underscore equivalence.
 * @param left Terminated borrowed string.
 * @param right Terminated borrowed string.
 * @return Equality; no ownership or synchronization effects.
 */
bool parameter_name_equal(const char *left,const char *right)
{
    for(;;++left,++right) {
        char a=*left=='-' ? '_' : *left,b=*right=='-' ? '_' : *right;
        if(a!=b)return false;
        if(!a)return true;
    }
}
/** @brief Parse base-0 integers with one optional terminal newline.
 * @param value Borrowed terminated input.
 * @param bits Destination width, 32 or 64.
 * @param signed_type Whether a minus sign is valid.
 * @param result Required output, unchanged on error.
 * @return Zero or EINVAL/ERANGE; no locks, allocation or writes to the target.
 */
static int parameter_number(const char *value,unsigned bits,bool signed_type,uint64_t *result)
{
    if(!value || !*value)return -EINVAL;
    bool negative=*value=='-';
    if(*value=='-' || *value=='+')++value;
    if(!*value || *value=='\n')return -EINVAL;
    if(negative && !signed_type)return -ERANGE;
    unsigned base=10;
    if(value[0]=='0') {
        base=8;
        if(value[1]=='x' || value[1]=='X'){base=16;value+=2;if(!*value || *value=='\n')return -EINVAL;}
    }
    uint64_t limit=signed_type ? ((UINT64_C(1)<<(bits-1))-(negative ? 0u : 1u)) :
                   (bits==64 ? UINT64_MAX : UINT32_MAX);
    uint64_t number=0;
    while(*value && !(*value=='\n' && !value[1])) {
        unsigned char c=(unsigned char)*value++;
        unsigned digit=c>='0' && c<='9' ? (unsigned)(c-'0') :
            c>='a' && c<='f' ? (unsigned)(c-'a'+10) :
            c>='A' && c<='F' ? (unsigned)(c-'A'+10) : 255u;
        if(digit>=base)return -EINVAL;
        if(digit>limit || number>(limit-digit)/base)return -ERANGE;
        number=number*base+digit;
    }
    *result=negative ? 0-number : number;return 0;
}
/** @brief Apply Linux kstrtobool's leading-byte/on/off grammar.
 * @param value Borrowed string, NULL denotes the bare true option.
 * @param out Required output, unchanged on error.
 * @return Zero or EINVAL; no allocation, locks or references.
 */
static int parameter_boolean(const char *value,bool *out)
{
    if(!value){*out=true;return 0;}
    switch(*value) {
    case '1':case 'y':case 'Y':case 't':case 'T':*out=true;return 0;
    case '0':case 'n':case 'N':case 'f':case 'F':*out=false;return 0;
    case 'o':case 'O':
        if(value[1]=='n' || value[1]=='N'){*out=true;return 0;}
        if(value[1]=='f' || value[1]=='F'){*out=false;return 0;}
    }
    return -EINVAL;
}
/** @brief Set one validated parameter, preserving prior data on failure.
 * @param values Exclusively owned table and allocation ledger.
 * @param parameter Retained destination descriptor.
 * @param value Terminated input, NULL only for a bare boolean.
 * @return Zero or negative errno. Task/execution ownership serializes access;
 * never allocates under a registry lock or frees a module-owned literal.
 */
int parameter_value_set(struct parameter_values *values,const struct module_parameter *parameter,const char *value)
{
    if(!values || !parameter || !parameter->value)return -EINVAL;
    size_t width=parameter_value_width(parameter);if(!width)return -EINVAL;
    if(parameter->type==MODULE_PARAMETER_charp) {
        if(!value)return -EINVAL;
        size_t length=0;while(value[length])if(++length>1024)return -ENOSPC;
        struct parameter_string *fresh=kernel_malloc(sizeof(*fresh)+length+1);
        if(!fresh)return -ENOMEM;
        fresh->parameter=parameter;
        for(size_t i=0;i<=length;++i)fresh->bytes[i]=value[i];
        struct parameter_string **at=&values->strings;
        while(*at && (*at)->parameter!=parameter)at=&(*at)->next;
        struct parameter_string *old=*at;
        fresh->next=old ? old->next : NULL;*at=fresh;
        char *text=fresh->bytes;__builtin_memcpy(parameter->value,&text,sizeof(text));
        kernel_free(old);return 0;
    }
    if(parameter->type==MODULE_PARAMETER_bool) {
        bool boolean;int rc=parameter_boolean(value,&boolean);
        if(!rc)__builtin_memcpy(parameter->value,&boolean,sizeof(boolean));
        return rc;
    }
    uint64_t number=0;
    bool signed_type=parameter->type==MODULE_PARAMETER_int || parameter->type==MODULE_PARAMETER_long;
    int rc=parameter_number(value,(unsigned)(width*8),signed_type,&number);
    if(!rc)__builtin_memcpy(parameter->value,&number,width);
    return rc;
}
/** @brief Generate decimal digits without signed-minimum overflow.
 * @param out Caller-owned buffer with room for 21 bytes.
 * @param number Unsigned magnitude.
 * @param negative Whether to prepend a minus sign.
 * @return Byte length; no allocation or locks, buffer is not terminated.
 */
static size_t parameter_decimal(char *out,uint64_t number,bool negative)
{
    char reverse[20];size_t count=0,length=0;
    do{reverse[count++]=(char)('0'+number%10);number/=10;}while(number);
    if(negative)out[length++]='-';
    while(count)out[length++]=reverse[--count];
    return length;
}
/** @brief Read a newline-terminated parameter stream at arbitrary byte offsets.
 * @param parameter Validated descriptor with retained backing.
 * @param offset Byte offset; beyond EOF succeeds with zero bytes.
 * @param buffer Writable output, NULL only for zero capacity.
 * @param capacity Output byte bound.
 * @param written Required byte result, cleared even on error.
 * @return Zero or negative errno. Caller serializes parameter access under
 * execution ownership; no allocation, locks, callbacks or reference changes.
 */
int parameter_value_read(const struct module_parameter *parameter,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written)
{
    if(!written)return -EINVAL;
    *written=0;
    if(!parameter || !parameter->value || (!buffer && capacity))return -EINVAL;
    char scalar[24];const char *text=scalar;size_t length=0;
    if(parameter->type==MODULE_PARAMETER_charp) {
        __builtin_memcpy(&text,parameter->value,sizeof(text));
        if(!text)text="";
        while(text[length])++length;
    } else if(parameter->type==MODULE_PARAMETER_bool) {
        bool boolean;__builtin_memcpy(&boolean,parameter->value,sizeof(boolean));scalar[length++]=boolean ? 'Y' : 'N';
    } else {
        uint64_t number=0;size_t width=parameter_value_width(parameter);if(!width)return -EINVAL;
        __builtin_memcpy(&number,parameter->value,width);
        bool negative=false;
        if(parameter->type==MODULE_PARAMETER_int) {
            int32_t signed_value=(int32_t)number;negative=signed_value<0;
            number=negative ? 0-(uint64_t)(int64_t)signed_value : (uint64_t)signed_value;
        } else if(parameter->type==MODULE_PARAMETER_long && (int64_t)number<0){negative=true;number=0-number;}
        length=parameter_decimal(scalar,number,negative);
    }
    bool newline=!length || text[length-1]!='\n';
    size_t size=length+(newline ? 1u : 0u);
    if(offset>=size)return 0;
    uint32_t count=0;
    while(count<capacity && offset<size){((char *)buffer)[count++]=offset<length ? text[offset] : '\n';++offset;}
    *written=count;return 0;
}
/** @brief Release only strings allocated by this parameter table.
 * @param values Exclusively cleanup-owned table, nullable; becomes empty.
 * Task context after exit and reference drain, with no registry locks held.
 */
void parameter_values_release(struct parameter_values *values)
{
    if(!values)return;
    while(values->strings){struct parameter_string *next=values->strings->next;kernel_free(values->strings);values->strings=next;}
}
