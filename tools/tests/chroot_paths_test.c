#define main permissions_fixture_main
#include "linux_permissions_test.c"
#undef main

int main(void)
{
    fixture_start();
    struct task task = {.cap_effective = UINT64_MAX};
    assert(!vfs_path_lookup(&fixture_ns,&fixture_root,&fixture_root,"/data",0,&task.fs_paths.root));
    strcpy(task.cwd, "/data/dir");
    char path[RELIEFOS_FS_PATH_LEN];
    assert(resolved_follow_path(&task, task.cwd, "/file", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    assert(resolved_follow_path(&task, task.cwd, "../../../../file", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    find("/data/link")->target = "/file";
    assert(resolved_follow_path(&task, task.cwd, "/link", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    find("/data/link")->target = "../../file";
    assert(resolved_follow_path(&task, task.cwd, "/link", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    assert(resolved_path(&task, task.cwd, "/link", path, sizeof(path), false, 0) == 0);
    assert(!strcmp(path, "/data/link"));
    assert(resolved_path(&task, task.cwd, "/../new", path, sizeof(path), false, VFS_LOOKUP_PARENT) == 0);
    assert(!strcmp(path, "/data/new"));
    /* Like Linux, chroot does not revoke preexisting outside directory FDs. */
    assert(resolved_follow_path(&task, "/", "data/file", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    struct task_fs_state shared = {0};
    vfs_path_get(&task.fs_paths.root,&shared.paths.root);
    vfs_path_put(&task.fs_paths.root);
    task.shared_fs = &shared;
    assert(resolved_follow_path(&task, "/", "/file", path, sizeof(path), false) == 0);
    assert(!strcmp(path, "/data/file"));
    vfs_path_put(&shared.paths.root);
    task.shared_fs=NULL;fixture_finish();
    puts("PASS rooted paths: absolute, relative, dotdot, links, dirfd and shared FS");
}
