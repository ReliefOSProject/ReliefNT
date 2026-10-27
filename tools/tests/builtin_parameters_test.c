/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/moduleparam.h>
#include "../../fs/sysfs/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static bool enabled;
static int count;
static char *label="literal";
static unsigned hidden;
module_param(enabled,bool,0644);
module_param(count,int,0444);
module_param(label,charp,0640);
module_param(hidden,uint,0);
static unsigned allocations;
void *kernel_malloc(size_t size){void *p=malloc(size);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
int fs_virtual_permissions(const char *p,const struct storage_node *n,struct reliefos_permissions *v,bool w)
{(void)p;(void)n;(void)v;(void)w;return -95;}
int main(void)
{
    char module[MODULE_NAME_LEN];struct module_parameter_info info;
    assert(module_parameter_builtin_exists("test-params"));
    assert(!module_parameter_builtin_at(0,module) && !strcmp(module,"test_params"));
    assert(module_parameter_builtin_at(1,module)==-2);
    assert(!module_parameters_boot("root=UUID=other unknown.x=1 test-params.enabled test_params.count=-123 test_params.label=\"with space\""));
    assert(enabled && count==-123 && !strcmp(label,"with space") && allocations==1);
    assert(!module_parameter_at("test-params",2,&info) && !strcmp(info.name,"label") && info.permissions==0640);
    assert(module_parameter_at("test_params",3,&info)==-2);
    uint32_t length;char bytes[40]={0};
    assert(!module_parameter_read("test_params","count",0,bytes,sizeof(bytes),&length) && length==5 && !memcmp(bytes,"-123\n",5));
    assert(!module_parameters_boot("test_params.count=5 -- test_params.count=7") && count==5);
    assert(module_parameters_boot("test_params.count=2147483648")==-34 && count==5);
    assert(!sysfs_parameter_truncate("/sys/module/test_params/parameters/label",0) && !strcmp(label,"with space"));
    assert(!sysfs_parameter_write("/sys/module/test_params/parameters/label",77,"replacement\n",12,&length) && length==12);
    assert(!strcmp(label,"replacement\n") && allocations==1);
    assert(sysfs_parameter_write("/sys/module/test_params/parameters/count",0,"7",1,&length)==-5 && !length);
    assert(sysfs_parameter_truncate("/sys/module/test_params/parameters/count",0)==-5);
    assert(sysfs_parameter_truncate("/sys/module/test_params/parameters/label",8)==-22);
    assert(sysfs_parameter_truncate("/sys/module/test_params/parameters/hidden",0)==-30);
    struct storage_node node={0};struct reliefos_permissions mode;
    assert(!sysfs_parameter_permissions("/sys/module/test_params/parameters/label",&node,&mode,false) && mode.mode==0640 && !mode.uid && !mode.gid);
    assert(sysfs_parameter_permissions("/sys/module/test_params/parameters/label",&node,&mode,true)==-30);
    const char *name;
    assert(!sysfs_parameter_key("/other/module/test_params/parameters/label",module,&name));
    assert(!sysfs_parameter_key("/sys/module/test_params/parameters/label/child",module,&name));
    assert(!module_parameter_write("test-params","enabled","off",3,&length) && !enabled);
    puts("PASS builtin MODULES=n table, boot quote/name grammar, visibility/modes, readonly store rejection, sysfs truncate and runtime strings");
}
