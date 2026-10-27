/* SPDX-License-Identifier: Apache-2.0 */
#include "../../kernel/params/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static bool fail_heap;
static unsigned live_heap;
void *kernel_malloc(size_t bytes){if(fail_heap)return NULL;void *p=malloc(bytes);if(p)++live_heap;return p;}
void kernel_free(void *p){if(p){assert(live_heap);--live_heap;free(p);}}
int main(void)
{
    bool enabled=false;int number=7;unsigned int count=9;long large=0;unsigned long full=0;char *label="default";
    struct module_parameter parameters[]={
        {"enabled",&enabled,MODULE_PARAMETER_bool,0644},{"number",&number,MODULE_PARAMETER_int,0444},
        {"count",&count,MODULE_PARAMETER_uint,0644},{"large",&large,MODULE_PARAMETER_long,0644},
        {"full",&full,MODULE_PARAMETER_ulong,0644},{"label",&label,MODULE_PARAMETER_charp,0644}};
    struct parameter_values values={.records=parameters,.count=6};
    assert(!parameter_value_set(&values,&parameters[0],NULL) && enabled);
    assert(!parameter_value_set(&values,&parameters[0],"off\n") && !enabled);
    assert(!parameter_value_set(&values,&parameters[0],"true") && enabled);
    assert(parameter_value_set(&values,&parameters[0],"2")==-22 && enabled);
    assert(!parameter_value_set(&values,&parameters[1],"-2147483648\n") && number==(-2147483647-1));
    assert(parameter_value_set(&values,&parameters[1],"2147483648")==-34 && number==(-2147483647-1));
    assert(!parameter_value_set(&values,&parameters[2],"0xffffffff\n") && count==4294967295u);
    assert(parameter_value_set(&values,&parameters[2],"-1")==-34 && count==4294967295u);
    assert(!parameter_value_set(&values,&parameters[3],"-9223372036854775808") && large==(-9223372036854775807L-1));
    assert(!parameter_value_set(&values,&parameters[4],"18446744073709551615") && full==~0UL);
    assert(parameter_value_set(&values,&parameters[4],"18446744073709551616")==-34 && full==~0UL);
    assert(!parameter_value_set(&values,&parameters[5],"hello world\n") && !strcmp(label,"hello world\n") && live_heap==1);
    char *before=label;fail_heap=true;
    assert(parameter_value_set(&values,&parameters[5],"lost")==-12 && label==before && live_heap==1);fail_heap=false;
    assert(!parameter_value_set(&values,&parameters[5],"next") && !strcmp(label,"next") && live_heap==1);
    char huge[1026];memset(huge,'x',sizeof(huge));huge[1025]=0;
    assert(parameter_value_set(&values,&parameters[5],huge)==-28 && !strcmp(label,"next"));
    huge[1024]=0;assert(!parameter_value_set(&values,&parameters[5],huge) && strlen(label)==1024 && live_heap==1);
    const char *expected[]={"Y\n","-2147483648\n","4294967295\n","-9223372036854775808\n","18446744073709551615\n"};
    for(unsigned p=0;p<5;++p) {
        char bytes[80]={0};uint32_t length=0;
        assert(!parameter_value_read(&parameters[p],0,bytes,sizeof(bytes),&length) && length==strlen(expected[p]) && !memcmp(bytes,expected[p],length));
        for(unsigned i=0;i<length;++i){char byte;uint32_t got;assert(!parameter_value_read(&parameters[p],i,&byte,1,&got) && got==1 && byte==expected[p][i]);}
    }
    assert(!parameter_value_set(&values,&parameters[5],"newline\n"));char bytes[40]={0};uint32_t length=0;
    assert(!parameter_value_read(&parameters[5],0,bytes,sizeof(bytes),&length) && length==8 && !memcmp(bytes,"newline\n",8));
    parameter_values_release(&values);assert(!live_heap);
    puts("PASS native parameter integer bounds, Linux booleans/charp limit, newline/short reads and OOM-preserving string ownership");
}
