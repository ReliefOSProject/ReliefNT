/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include "storage_backend.h"
#include "../../block/core/storage_internal.h"
#include "../security/sidecar.h"

/* A sidecar operation owns the exact parent inode via its calling path. These
 * callbacks select only that volume; no namespace pathname participates
 * in persistent metadata I/O. */
struct bound_sidecar {struct storage_volume *volume;struct storage_node parent;};
#define BOUND_SIDECAR_LIMIT (16u*1024u*1024u)

static int bound_sidecar_node(struct bound_sidecar *bound,struct storage_node *node,bool create,bool *created)
{
    if(created)*created=false;
    int rc=storage_inode_refresh(&bound->parent);
    if(!rc)rc=storage_backend_lookup(bound->volume,&bound->parent,"LEONACL.SYS",node);
    if(rc!=-ENOENT || !create)return rc;
    rc=storage_cluster_new_file(bound->volume,&bound->parent,"LEONACL.SYS",node);
    if(!rc && created)*created=true;
    return rc;
}
static int bound_sidecar_read(void *context,const void **data,size_t *length)
{
    struct bound_sidecar *bound=context;struct storage_node node;
    *data=NULL;*length=0;
    int rc=bound_sidecar_node(bound,&node,false,NULL);if(rc)return rc;
    if(node.type!=RELIEFOS_FS_TYPE_FILE || node.size>BOUND_SIDECAR_LIMIT)return -EIO;
    struct storage_inode_ref *pin=NULL;rc=storage_inode_get(&node,&pin);if(rc)return rc;
    uint32_t pages=(uint32_t)((node.size+4095)/4096);if(!pages)pages=1;
    uint64_t address=mm_alloc_pages(pages);void *bytes=address ? mm_phys_to_virt(address) : NULL;
    if(!bytes)rc=-ENOMEM;
    uint32_t offset=0;
    while(!rc && offset<node.size) {
        uint32_t count=0;rc=storage_read_node(&node,offset,(char *)bytes+offset,(uint32_t)node.size-offset,&count);
        if(!rc && !count)rc=-EIO;
        offset+=count;
    }
    (void)storage_inode_put(pin);
    if(rc){if(address)mm_free_pages(address,pages);return rc;}
    *data=bytes;*length=(size_t)node.size;return 0;
}
static int bound_sidecar_write(void *context,const void *data,uint32_t length)
{
    struct bound_sidecar *bound=context;struct storage_node node;
    int rc=bound_sidecar_node(bound,&node,true,NULL);if(rc)return rc;
    if(node.type!=RELIEFOS_FS_TYPE_FILE)return -EIO;
    struct storage_inode_ref *pin=NULL;rc=storage_inode_get(&node,&pin);if(rc)return rc;
    rc=storage_inode_write_get(pin);
    if(!rc) {
        uint32_t written=0;storage_begin_mutation();
        rc=storage_write_held_node(&node,0,data,length,&written);
        if(!rc && written!=length)rc=-EIO;
        if(!rc)rc=storage_truncate_held_node(&node,length);
        storage_inode_write_put(pin);
    }
    (void)storage_inode_put(pin);return rc;
}
int storage_backend_store_child(struct storage_volume *volume,const struct storage_node *parent,
                                   const char *name,const struct reliefos_permissions *value)
{
    if(!volume || !parent || !name || !value)return -EINVAL;
    struct bound_sidecar bound={volume,*parent};
    struct storage_sidecar_io io={&bound,bound_sidecar_read,bound_sidecar_write};
    struct reliefos_permissions stored=*value;
    return storage_sidecar_permissions_at(&io,name,"/",&stored,true);
}

int storage_backend_permissions(void *context,const struct vfs_path *path,
                                  struct reliefos_permissions *value,bool write)
{
    struct storage_volume *volume=context;
    if(!vfs_path_valid(path) || !value || !volume || path->dentry->node.volume_id!=volume->volume_id)return -EINVAL;
    const struct storage_node *node=&path->dentry->node;
    if(node->flags&(STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_EXT2))
        return storage_inode_permissions(node,value,write);
    if(volume->filesystem==STORAGE_FILESYSTEM_ISO9660) {
        if(write)return -EROFS;
        *value=(struct reliefos_permissions){0555,0,0};return 0;
    }
    if(!(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)))return -EOPNOTSUPP;
    struct storage_inode_ref *pin=path->dentry->backend_pin;if(!pin)return -ESTALE;
    if(!write && pin->permissions_valid){*value=pin->permissions;return 0;}
    if(pin->unlinked) {
        if(!pin->permissions_valid)return -ESTALE;
        pin->permissions=*value;return 0;
    }
    char intrinsic[VFS_PATH_MAX];int rc=vfs_path_backend_name(path,intrinsic,sizeof(intrinsic));if(rc)return rc;
    struct vfs_dentry *parent=path->dentry->parent;
    struct bound_sidecar bound={volume,parent ? parent->node : *node};
    struct storage_sidecar_io io={&bound,bound_sidecar_read,bound_sidecar_write};
    rc=storage_sidecar_permissions_at(&io,parent ? path->dentry->name : ".",intrinsic,value,write);
    if(!rc){pin->permissions=*value;pin->permissions_valid=true;}
    return rc;
}

/* Removing/replacing an otherwise empty FAT-family directory also unlinks
 * its internal sidecar. An O_PATH holder may not have queried its mode yet.
 * Populate that same inode's real authority before its name disappears. */
int storage_backend_prime_metadata(const struct vfs_path *directory)
{
    const struct storage_node *node=vfs_path_node(directory);
    if(!node)return -EINVAL;
    if(!(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)))return 0;
    struct vfs_path metadata={0};int rc=vfs_path_lookup_child(directory,"LEONACL.SYS",&metadata);
    if(rc==-ENOENT)return 0;
    if(!rc) {
        struct reliefos_permissions value;rc=vfs_path_permissions(&metadata,&value,false);
    }
    vfs_path_put(&metadata);return rc;
}

struct storage_rename_metadata {
    struct storage_volume *volume;
    struct storage_node node,parent;
    struct storage_inode_ref *pin;
    bool writer,created;
    struct storage_sidecar_change images;
    struct storage_rename_metadata *next;
};

/* Preheld native inode and fixed-size valid images keep this write free of
 * heap/page allocation. Reservation runs before rename; commit writes exactly
 * the already allocated chain, without increasing the logical length. */
static int bound_rename_write(struct storage_rename_metadata *change,const void *bytes)
{
    struct storage_volume *previous=NULL;
    int rc=storage_select_node_volume(&change->node,&previous);if(rc)return rc;
    storage_begin_mutation();uint32_t written=0;
    rc=storage_cluster_write(&change->node,0,bytes,change->images.length,&written);
    if(!rc && written!=change->images.length)rc=-EIO;
    storage_restore_volume(previous);return rc;
}

int storage_backend_finish_rename_metadata(struct storage_rename_metadata *change,bool committed)
{
    int result=0;
    while(change) {
        struct storage_rename_metadata *next=change->next;
        if(change->writer)storage_inode_write_put(change->pin);
        if(change->created && !committed) {
            int rc=storage_cluster_drop_new(change->volume,&change->parent,"LEONACL.SYS",&change->node);
            if(rc && !result)result=rc;
        }
        int close=storage_inode_put(change->pin);if(close && !result)result=close;
        storage_sidecar_change_release(&change->images);kernel_free(change);change=next;
    }
    return result;
}

static int bound_rename_hold(struct storage_rename_metadata *change,struct bound_sidecar *bound)
{
    int rc=bound_sidecar_node(bound,&change->node,true,&change->created);
    change->parent=bound->parent;
    if(!rc && change->node.type!=RELIEFOS_FS_TYPE_FILE)rc=-EIO;
    if(!rc)rc=storage_inode_get(&change->node,&change->pin);
    if(!rc){rc=storage_inode_write_get(change->pin);if(!rc)change->writer=true;}
    return rc;
}

int storage_backend_prepare_rename_metadata(struct storage_volume *volume,const struct vfs_path *source,
                                               const struct vfs_path *parent,const char *name,
                                               const struct reliefos_permissions *value,
                                               struct storage_rename_metadata **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!volume || !vfs_path_valid(source) || !source->dentry->parent || !vfs_path_valid(parent) || !name || !value)return -EINVAL;
    struct storage_node directory=source->dentry->parent->node;
    if(storage_text_eq_ci(vfs_path_name(source),"LEONACL.SYS") || storage_text_eq_ci(name,"LEONACL.SYS"))return -EPERM;
    struct storage_rename_metadata *change=kernel_malloc(sizeof(*change));if(!change)return -ENOMEM;
    *change=(struct storage_rename_metadata){.volume=volume};
    struct bound_sidecar bound={volume,directory},destination={volume,*vfs_path_node(parent)};
    struct storage_sidecar_io source_io={&bound,bound_sidecar_read,bound_sidecar_write};
    struct storage_sidecar_io destination_io={&destination,bound_sidecar_read,bound_sidecar_write};
    bool moving=directory.first_cluster!=destination.parent.first_cluster;
    int rc=0;
    if(moving) {
        change->next=kernel_malloc(sizeof(*change->next));
        if(!change->next)rc=-ENOMEM;
        else {
            *change->next=(struct storage_rename_metadata){.volume=volume};
            rc=storage_sidecar_prepare_move(&source_io,vfs_path_name(source),&destination_io,name,value,
                &change->images,&change->next->images);
        }
    } else rc=storage_sidecar_prepare_rename(&source_io,vfs_path_name(source),name,value,&change->images);
    if(!rc)rc=bound_rename_hold(change,&bound);
    if(!rc && moving)rc=bound_rename_hold(change->next,&destination);
    /* Reserve every buffer, inode/write lease and valid old-image file space
     * before native rename. Both directories still expose their old records. */
    if(!rc)rc=bound_rename_write(change,change->images.before);
    if(!rc && moving)rc=bound_rename_write(change->next,change->next->images.before);
    if(rc){int cleanup=storage_backend_finish_rename_metadata(change,false);return cleanup ? cleanup : rc;}
    *out=change;return 0;
}

int storage_backend_commit_rename_metadata(struct storage_rename_metadata *change)
{
    if(!change || change->node.size!=change->images.length)return -EINVAL;
    int rc=0;
    if(change->next) {
        if(change->next->node.size!=change->next->images.length)return -EINVAL;
        rc=bound_rename_write(change->next,change->next->images.after);
    }
    return rc ? rc : bound_rename_write(change,change->images.after);
}
