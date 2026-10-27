#include "permission_graph_fixture.h"

int main(void)
{
    fixture_start();
    struct task owner = {.uid = 100, .euid = 100, .fsuid = 100,
        .gid = 200, .egid = 200, .fsgid = 200, .umask = 0027};
    struct task group = {.uid = 101, .euid = 101, .fsuid = 101,
        .gid = 200, .egid = 200, .fsgid = 200};
    struct task other = {.uid = 102, .euid = 102, .fsuid = 102,
        .gid = 300, .egid = 300, .fsgid = 300};
    struct task root = {.cap_effective = (1ULL << (CAP_LAST_CAP + 1)) - 1,
        .cap_permitted = (1ULL << (CAP_LAST_CAP + 1)) - 1};
    struct storage_node recording = {.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_AUDIO_PCM,
        .first_cluster = STORAGE_DEV_KIND_AUDIO, .volume_id = 0x123401};
    assert(!fs_virtual_chmod(&root, "/dev/snd/pcmC0D0c", &recording, 0660));
    assert(!fs_virtual_chown(&root, "/dev/snd/pcmC0D0c", &recording, 100, 200));
    assert(!file_check(&owner, "/dev/snd/pcmC0D0c", FS_ACCESS_READ, false));
    assert(!file_check(&group, "/dev/snd/pcmC0D0c", FS_ACCESS_READ, false));
    assert(file_check(&other, "/dev/snd/pcmC0D0c", FS_ACCESS_READ, false) == -EACCES);
    assert(fs_virtual_chmod(&other, "/dev/snd/pcmC0D0c", &recording, 0666) == -EPERM);
    assert(!file_check(&root, "/dev/snd/pcmC0D0c", FS_ACCESS_READ, false));
    puts("PASS recording node uses actual owner/group DAC and refuses unrelated users");

    struct storage_node render_node = {
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = STORAGE_DEV_KIND_DRM_RENDERD128,
    };
    struct reliefos_permissions render_permissions;
    assert(fs_virtual_permissions("renderD128", &render_node, &render_permissions, false) == 0);
    assert(render_permissions.mode == 0660 && render_permissions.uid == 0 &&
           render_permissions.gid == 44);
    struct task video_client = {
        .uid = 1000, .euid = 1000, .fsuid = 1000,
        .gid = 44, .egid = 44, .fsgid = 44,
    };
    struct task unrelated_client = {
        .uid = 1001, .euid = 1001, .fsuid = 1001,
        .gid = 300, .egid = 300, .fsgid = 300,
    };
    assert(file_check(&video_client, "/dev/dri/renderD128",
                      FS_ACCESS_READ | FS_ACCESS_WRITE, false) == 0);
    assert(file_check(&unrelated_client, "/dev/dri/renderD128",
                      FS_ACCESS_READ, false) == -RELIEFOS_EACCES);
    puts("PASS render node defaults to char-device DAC owner root group 44 mode 0660");

    entries[2].value.mode = 0755;
    assert(!vfs_path_remount(&fixture_root,MS_NOEXEC));
    assert(file_check(&root, "/data/file", FS_ACCESS_EXEC, false) == -RELIEFOS_EACCES);
    assert(file_check(&root, "/data/dir", FS_ACCESS_EXEC, false) == 0);
    assert(!vfs_path_remount(&fixture_root,0));
    entries[2].value.mode = 0640;
    struct storage_node pty = {.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_PTY | STORAGE_NODE_FLAG_DEV_NODE};
    assert(file_check(&owner, "/dev/pts/1", FS_ACCESS_READ | FS_ACCESS_WRITE, false) == 0);
    assert(file_check(&other, "/dev/pts/1", FS_ACCESS_READ, false) == -13);
    assert(fs_virtual_chmod(&other, "/dev/pts/1", &pty, 0666) == -1);
    assert(fs_virtual_chown(&owner, "/dev/pts/1", &pty, 101, UINT32_MAX) == -1);
    assert(fs_virtual_chown(&root, "/dev/pts/1", &pty, other.fsuid, other.fsgid) == 0);
    assert(file_check(&other, "/dev/pts/1", FS_ACCESS_READ | FS_ACCESS_WRITE, false) == 0);
    assert(file_check(&owner, "/dev/pts/1", FS_ACCESS_READ, false) == -13);

    struct storage_node regular={.type=RELIEFOS_FS_TYPE_FILE};
    assert(fs_virtual_chmod(&root,"/data/file",&regular,0600)==-RELIEFOS_EOPNOTSUPP);
    assert(fs_virtual_chown(&root,"/data/file",&regular,100,200)==-RELIEFOS_EOPNOTSUPP);
    assert(fs_virtual_chmod(&root,"/dev/pts/1",NULL,0600)==-RELIEFOS_EINVAL);
    assert(fs_virtual_chown(&root,NULL,&pty,100,200)==-RELIEFOS_EINVAL);
    puts("PASS implicit virtual endpoint mutations never fall back to a disk pathname");
    const char *file = "/data/file";
    /* UID/GID zero are real filesystem identities, never unset sentinels. */
    struct task fsroot = other;
    fsroot.fsuid = 0;
    entries[2].value = (struct reliefos_permissions){0600, 0, 0};
    assert(file_check(&fsroot, file, 6, false) == 0);
    assert(file_check(&fsroot, file, 6, true) == -13);
    struct task fsgroup = other;
    fsgroup.fsgid = 0;
    entries[2].value.mode = 0040;
    assert(file_check(&fsgroup, file, 4, false) == 0);
    assert(file_check(&fsgroup, file, 4, true) == -13);
    entries[2].value = (struct reliefos_permissions){0640, 100, 200};
    struct task dropped_root = {0};
    assert(file_check(&dropped_root, file, 4, false) == -13);
    assert(file_chmod(&dropped_root, file, VFS_LOOKUP_FOLLOW, 0600) == -1);
    assert(file_chown(&dropped_root, file, VFS_LOOKUP_FOLLOW, 0, 0) == -1);
    struct task delegated = other;
    delegated.cap_effective = 1ULL << CAP_DAC_READ_SEARCH;
    assert(file_check(&delegated, file, 4, false) == 0);
    assert(file_check(&delegated, file, 2, false) == -13);
    assert(file_check(&delegated, file, 4, true) == -13);
    delegated.cap_effective = 1ULL << CAP_DAC_OVERRIDE;
    assert(file_check(&delegated, file, 2, false) == 0);
    assert(file_check(&delegated, file, 1, false) == -13);
    struct task fsowner = other;
    fsowner.fsuid = owner.fsuid;
    assert(file_chmod(&fsowner, file, VFS_LOOKUP_FOLLOW, 0640) == 0);
    assert(file_check(&owner, file, 6, false) == 0);
    assert(file_check(&group, file, 4, false) == 0);
    assert(file_check(&group, file, 2, false) == -13);
    assert(file_check(&other, file, 4, false) == -13);
    assert(file_chmod(&other, file, VFS_LOOKUP_FOLLOW, 0777) == -1);
    assert(file_chown(&other, file, VFS_LOOKUP_FOLLOW, UINT32_MAX, UINT32_MAX) == 0);
    assert(file_chmod(&owner, "/data/missing", VFS_LOOKUP_FOLLOW, 0777) == -2);
    assert(file_chmod(&owner, file, VFS_LOOKUP_FOLLOW, 0600) == 0);
    assert(file_check(&group, file, 4, false) == -13);
    assert(file_check(&root, file, 1, false) == -13);
    assert(file_check(&root, file, 6, false) == 0);
    assert(file_chmod(&owner, file, VFS_LOOKUP_FOLLOW, 0004) == 0);
    assert(file_check(&owner, file, 4, false) == -13);
    assert(file_check(&other, file, 4, false) == 0);
    entries[1].value.mode = 0700;
    assert(file_check(&other, file, 4, false) == -13);
    char resolved[RELIEFOS_FS_PATH_LEN];
    assert(resolved_follow_path(&owner, "/", "/jump/../file", resolved, sizeof(resolved), false) == 0);
    assert(!strcmp(resolved, "/data/file"));
    assert(resolved_follow_path(&owner, "/", "/absolute", resolved, sizeof(resolved), false) == 0);
    assert(!strcmp(resolved, "/data/file"));
    assert(resolved_follow_path(&other, "/", "/absolute", resolved, sizeof(resolved), false) == -13);
    assert(resolved_follow_path(&owner, "/", "/loop", resolved, sizeof(resolved), false) == -40);
    assert(resolved_path(&owner, "/", "/loop", resolved, sizeof(resolved), false, 0) == 0);
    assert(!strcmp(resolved, "/loop"));
    assert(resolved_path(&owner, "/", "/jump/../link", resolved, sizeof(resolved), false, 0) == 0);
    assert(!strcmp(resolved, "/data/link"));
    assert(resolved_path(&owner, "/", "/jump/", resolved, sizeof(resolved), false, 0) == 0);
    assert(!strcmp(resolved, "/data/dir"));
    assert(resolved_path(&owner, "/", "/jump/", resolved, sizeof(resolved), false, VFS_LOOKUP_PARENT) == 0);
    assert(!strcmp(resolved, "/jump/"));
    assert(resolved_path(&owner, "/", "/dangling", resolved, sizeof(resolved), false, VFS_LOOKUP_PARENT) == 0);
    assert(!strcmp(resolved, "/dangling"));
    assert(resolved_follow_path(&owner, "/", "/dangling", resolved, sizeof(resolved), false) == 0);
    assert(!strcmp(resolved, "/missing"));
    assert(resolved_follow_path(&owner, "/", "/data/file/", resolved, sizeof(resolved), false) == -20);
    char chain[256] = "";
    for (unsigned i = 0; i < 40; ++i) strcat(chain, "/self");
    assert(resolved_follow_path(&owner, "/", chain, resolved, sizeof(resolved), false) == 0);
    assert(!strcmp(resolved, "/"));
    strcat(chain, "/self");
    assert(resolved_follow_path(&owner, "/", chain, resolved, sizeof(resolved), false) == -40);
    assert(resolved_follow_path(&other, "/", "/data/../", resolved, sizeof(resolved), false) == -13);
    assert(resolved_follow_path(&owner, "/", "/data/../data/file", resolved, sizeof(resolved), false) == 0);
    assert(!strcmp(resolved, file));
    assert(resolved_follow_path(&root, "/", "/absent/../data", resolved, sizeof(resolved), false) == -2);
    assert(resolved_follow_path(&owner, "/data", "file/../file", resolved, sizeof(resolved), false) == -20);
    assert(resolved_follow_path(&owner, "/data", "./file", resolved, sizeof(resolved), false) == 0);
    struct task access_root = other;
    access_root.uid = 0;
    access_root.cap_permitted = 1ULL << CAP_DAC_OVERRIDE;
    assert(resolved_follow_path(&access_root, "/", "/data/../", resolved, sizeof(resolved), true) == 0);
    assert(resolved_follow_path(&access_root, "/", "/data/../", resolved, sizeof(resolved), false) == -13);
    entries[1].value.mode = 01777;
    assert(mutation_parent(&other, file, true) == -1);
    assert(mutation_parent(&owner, file, true) == 0);
    assert(file_chown(&owner, file, VFS_LOOKUP_FOLLOW, 101, UINT32_MAX) == -1);
    assert(file_chown(&owner, file, VFS_LOOKUP_FOLLOW, UINT32_MAX, 300) == -1);
    assert(file_chmod(&root, file, VFS_LOOKUP_FOLLOW, 06755) == 0);
    assert(file_chown(&root, file, VFS_LOOKUP_FOLLOW, 102, 300) == 0);
    assert(entries[2].value.uid == 102 && entries[2].value.gid == 300);
    assert(entries[2].value.mode == 0755);
    assert(file_chown(&other, file, VFS_LOOKUP_FOLLOW, UINT32_MAX, UINT32_MAX) == 0);
    assert(entries[2].value.uid == 102 && entries[2].value.gid == 300);
    assert(!create_file(&owner,"/data/created-file",LINUX_S_IFREG,0666,NULL));
    struct fixture *created=find("/data/created-file");
    assert(created->value.mode==0640 && created->value.uid==100 && created->value.gid==200);
    entries[2].value=(struct reliefos_permissions){0640,100,200};
    entries[1].value = (struct reliefos_permissions){02777, 100, 700};
    assert(!create_file(&owner,"/data/created-link",LINUX_S_IFLNK,0777,"file"));
    assert(find("/data/created-link")->value.mode == 0777 && find("/data/created-link")->value.gid == 700);
    assert(file_chmod(&owner, "/data/created-link", 0, 0600) == -95);
    assert(file_chown(&root, "/data/created-link", 0, 123, 456) == 0);
    assert(find("/data/created-link")->value.uid == 123 && find("/data/created-link")->value.mode == 0777);
    assert(entries[2].value.uid == 100 && entries[2].value.mode == 0640);
    entries[1].value = (struct reliefos_permissions){01777, 100, 200};
    struct task elevated = owner;
    elevated.uid = 102;
    assert(file_check(&elevated, file, 6, false) == 0);
    assert(file_check(&elevated, file, 6, true) == -13);
    uint32_t gids[] = {400, 200, 400, 300};
    assert(task_groups_set(&other, gids, 4) == 0);
    assert(other.groups->count == 4 && other.groups->ids[0] == 200 && other.groups->ids[1] == 300);
    assert(other.groups->ids[2] == 400 && other.groups->ids[3] == 400);
    assert(file_check(&other, file, 4, false) == 0);
    assert(file_check(&other, file, 2, false) == -13);
    struct task child = other;
    task_groups_retain(child.groups);
    assert(task_groups_set(&other, NULL, 0) == 0 && !other.groups);
    assert(file_check(&child, file, 4, false) == 0);
    task_groups_release(&child);
    assert(!child.groups);
    fixture_finish();
    puts("PASS Unix permissions: owner/group/other priority, search, sticky, chmod, chown, umask, access IDs");
}
