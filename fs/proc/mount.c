/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/pseudo_fs.h>
#include <reliefnt/permissions.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <linux/mount.h>

static int proc_key_identity(const char *name,void **out,bool *magic)
{
    struct task_identity *identity=NULL;
    int rc=proc_mount_node_identity(name,&identity,magic);*out=identity;return rc;
}
static bool proc_key_alive(void *identity)
{return sched_task_identity_task(identity)!=NULL;}
static void proc_key_put(void *identity)
{sched_task_identity_put(identity);}

int proc_create_vfs_mount(struct vfs_mount **out)
{
    static const struct vfs_pseudo_provider provider={.root="/proc",.filesystem="proc",.lookup=proc_lookup,.mount_flags=MS_RDONLY,
        .read=proc_read,.readdir=proc_readdir,.readlink=proc_readlink,
        .get_identity=proc_key_identity,.identity_alive=proc_key_alive,.put_identity=proc_key_put,
        .follow=proc_mount_magic_target,.permissions=fs_virtual_permissions,
        .open=proc_file_open,.read_open=proc_file_read,.close=proc_file_close};
    return vfs_pseudo_mount_new(&provider,out);
}
