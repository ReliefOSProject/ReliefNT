#include "../internal.h"

/**
 * Require driver management.
 * @return The value or status produced by the operation.
 */
static int require_driver_management(void);

/**
 * Require background service.
 * @return The value or status produced by the operation.
 */
static int require_background_service(void);

/**
 * Startup release file.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
static void startup_release_file(const void *data, size_t len);

/**
 * Startup command is well formed.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_is_well_formed(const struct reliefos_startup_command *command);

/**
 * Startup db is well formed.
 * @param db Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_db_is_well_formed(const struct startup_db *db);

/**
 * Startup denial db is well formed.
 * @param db Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_denial_db_is_well_formed(const struct startup_denial_db *db);

/**
 * Startup db load.
 */
static void startup_db_load(void);

/**
 * Startup db save.
 * @return The value or status produced by the operation.
 */
static int startup_db_save(void);

/**
 * Startup denial db load.
 */
static void startup_denial_db_load(void);

/**
 * Startup denial db save.
 * @return The value or status produced by the operation.
 */
static int startup_denial_db_save(void);

/**
 * Startup text eq.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_text_eq(const char *a, const char *b);

/**
 * Startup command equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_equal(const struct reliefos_startup_command *a,
                                 const struct reliefos_startup_command *b);

/**
 * Startup command validate.
 * @param command Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_validate(struct reliefos_startup_command *command,
                                    const struct task *task);

/**
 * Startup can manage uid.
 * @param task Value supplied by the caller.
 * @param uid Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
static int startup_can_manage_uid(const struct task *task, uint32_t uid);

/**
 * Startup db find.
 * @param uid Identifier or flags controlling the operation.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_db_find(uint32_t uid, const struct reliefos_startup_command *command);

/**
 * Startup denial find.
 * @param uid Identifier or flags controlling the operation.
 * @param requester_path NUL-terminated text supplied by the caller.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_denial_find(uint32_t uid, const char *requester_path,
                               const struct reliefos_startup_command *command);

/**
 * Startup remember denial.
 * @param uid Identifier or flags controlling the operation.
 * @param requester_path NUL-terminated text supplied by the caller.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_remember_denial(uint32_t uid, const char *requester_path,
                                   const struct reliefos_startup_command *command);

/**
 * Startup request find.
 * @param id Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct startup_request_slot *startup_request_find(uint32_t id);

/**
 * Startup request reconcile.
 * @param slot Value supplied by the caller.
 */
static void startup_request_reconcile(struct startup_request_slot *slot);

/**
 * Startup request alloc.
 * @return The value or status produced by the operation.
 */
static struct startup_request_slot *startup_request_alloc(void);

/**
 * Startup add entry.
 * @param uid Identifier or flags controlling the operation.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_add_entry(uint32_t uid, const struct reliefos_startup_command *command);

/**
 * Startup dialog spawn.
 * @param slot Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_dialog_spawn(struct startup_request_slot *slot);

static struct startup_db startup_db_scratch;
static struct startup_denial_db startup_denial_db_scratch;
static struct startup_request_slot startup_requests[STARTUP_REQUEST_MAX];
static uint32_t startup_next_request_id = 1;
/**
 * Require driver management.
 * @return The value or status produced by the operation.
 */
static int require_driver_management(void)
{
    struct task *task = sched_current_task();
    return task_effective_role(task) == RELIEFOS_AUTH_ROLE_ADMIN
               ? 0
               : -RELIEFOS_EPERM;
}

/**
 * Task effective role.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t task_effective_role(const struct task *task)
{
    if (!task) {
        return RELIEFOS_AUTH_ROLE_NONE;
    }
    if (task->role == RELIEFOS_AUTH_ROLE_ADMIN ||
        (task->flags & TASK_FLAG_ELEVATED_ADMIN)) {
        return RELIEFOS_AUTH_ROLE_ADMIN;
    }
    return task->role;
}

/**
 * Require background service.
 * @return The value or status produced by the operation.
 */
static int require_background_service(void)
{
    struct task *task = sched_current_task();
    if (!task || !(task->flags & TASK_FLAG_SERVICE)) {
        return -RELIEFOS_EPERM;
    }
    return 0;
}

/**
 * Startup release file.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
static void startup_release_file(const void *data, size_t len)
{
    uint32_t pages = (uint32_t)((len + 4095U) / 4096U);
    if (data && pages) {
        mm_free_pages((uint64_t)(uintptr_t)data, pages);
    }
}

/**
 * Startup command is well formed.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_is_well_formed(const struct reliefos_startup_command *command)
{
    if (!command || !command->path[0] || command->argc > RELIEFOS_STARTUP_MAX_ARGS ||
        kernel_string_len_cap(command->path, sizeof(command->path)) >= sizeof(command->path)) {
        return 0;
    }
    for (uint32_t i = 0; i < command->argc; ++i) {
        if (kernel_string_len_cap(command->args[i], sizeof(command->args[i])) >=
            sizeof(command->args[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Startup db is well formed.
 * @param db Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_db_is_well_formed(const struct startup_db *db)
{
    if (!db || db->magic != STARTUP_DB_MAGIC || db->count > STARTUP_DB_ENTRY_MAX ||
        !db->next_id) {
        return 0;
    }
    for (uint32_t i = 0; i < db->count; ++i) {
        if (!db->entries[i].uid || !db->entries[i].entry.id ||
            !startup_command_is_well_formed(&db->entries[i].entry.command)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Startup denial db is well formed.
 * @param db Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_denial_db_is_well_formed(const struct startup_denial_db *db)
{
    if (!db || db->magic != STARTUP_DENIAL_DB_MAGIC || db->count > STARTUP_DENIAL_MAX) {
        return 0;
    }
    for (uint32_t i = 0; i < db->count; ++i) {
        if (!db->entries[i].uid ||
            kernel_string_len_cap(db->entries[i].requester_path,
                                  sizeof(db->entries[i].requester_path)) >=
                sizeof(db->entries[i].requester_path) ||
            !startup_command_is_well_formed(&db->entries[i].command)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Startup db load.
 */
static void startup_db_load(void)
{
    const void *data = 0;
    size_t len = 0;
    startup_db_scratch = (struct startup_db){0};
    startup_db_scratch.magic = STARTUP_DB_MAGIC;
    startup_db_scratch.next_id = 1;
    if (storage_read_file(STARTUP_DB_PATH, &data, &len) == 0 &&
        data && len == sizeof(startup_db_scratch)) {
        const struct startup_db *saved = (const struct startup_db *)data;
        if (startup_db_is_well_formed(saved)) {
            startup_db_scratch = *saved;
        }
    }
    startup_release_file(data, len);
}

/**
 * Startup db save.
 * @return The value or status produced by the operation.
 */
static int startup_db_save(void)
{
    (void)storage_mkdir("/var");
    (void)storage_mkdir("/var/lib");
    (void)storage_mkdir(RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS);
    return storage_write_file(STARTUP_DB_PATH, &startup_db_scratch,
                              sizeof(startup_db_scratch));
}

/**
 * Startup denial db load.
 */
static void startup_denial_db_load(void)
{
    const void *data = 0;
    size_t len = 0;
    startup_denial_db_scratch = (struct startup_denial_db){0};
    startup_denial_db_scratch.magic = STARTUP_DENIAL_DB_MAGIC;
    if (storage_read_file(STARTUP_DENIAL_DB_PATH, &data, &len) == 0 &&
        data && len == sizeof(startup_denial_db_scratch)) {
        const struct startup_denial_db *saved = (const struct startup_denial_db *)data;
        if (startup_denial_db_is_well_formed(saved)) {
            startup_denial_db_scratch = *saved;
        }
    }
    startup_release_file(data, len);
}

/**
 * Startup denial db save.
 * @return The value or status produced by the operation.
 */
static int startup_denial_db_save(void)
{
    (void)storage_mkdir("/var");
    (void)storage_mkdir("/var/lib");
    (void)storage_mkdir(RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS);
    return storage_write_file(STARTUP_DENIAL_DB_PATH, &startup_denial_db_scratch,
                              sizeof(startup_denial_db_scratch));
}

/**
 * Startup text eq.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_text_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i] && a[i] == b[i]) {
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

/**
 * Startup command equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_equal(const struct reliefos_startup_command *a,
                                 const struct reliefos_startup_command *b)
{
    if (!a || !b || a->argc != b->argc || !startup_text_eq(a->path, b->path)) {
        return 0;
    }
    for (uint32_t i = 0; i < a->argc; ++i) {
        if (!startup_text_eq(a->args[i], b->args[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Startup command validate.
 * @param command Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_command_validate(struct reliefos_startup_command *command,
                                    const struct task *task)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    struct reliefos_stat st;
    uint32_t exec_bytes;
    int ret;
    if (!command || !task || !command->path[0] ||
        command->argc > RELIEFOS_STARTUP_MAX_ARGS ||
        kernel_string_len_cap(command->path, sizeof(command->path)) >= sizeof(command->path)) {
        return -RELIEFOS_EINVAL;
    }
    for (uint32_t i = 0; i < command->argc; ++i) {
        uint32_t arg_len = kernel_string_len_cap(command->args[i], sizeof(command->args[i]));
        if (arg_len >= sizeof(command->args[i])) {
            return -RELIEFOS_EINVAL;
        }
    }
    ret = storage_resolve_path(sched_task_cwd(task), command->path, resolved, sizeof(resolved));
    if (ret < 0) {
        return -RELIEFOS_EINVAL;
    }
    ret = storage_stat_path(resolved, &st);
    if (ret < 0) {
        return ret == -2 ? -RELIEFOS_EINVAL : ret;
    }
    if (st.type != RELIEFOS_FS_TYPE_FILE) {
        return -RELIEFOS_EINVAL;
    }
    ret = fs_permissions_check(task, resolved, FS_ACCESS_EXEC, false);
    if (ret < 0) {
        return ret;
    }
    copy_text(command->path, sizeof(command->path), resolved);
    exec_bytes = kernel_string_len_cap(command->path, sizeof(command->path)) + 1U;
    for (uint32_t i = 0; i < command->argc; ++i) {
        exec_bytes += kernel_string_len_cap(command->args[i], sizeof(command->args[i])) + 1U;
    }
    if (exec_bytes > SCHED_EXEC_DATA_MAX) {
        return -RELIEFOS_EINVAL;
    }
    command->reserved = 0;
    for (uint32_t i = command->argc; i < RELIEFOS_STARTUP_MAX_ARGS; ++i) {
        command->args[i][0] = 0;
    }
    return 0;
}

/**
 * Startup can manage uid.
 * @param task Value supplied by the caller.
 * @param uid Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
static int startup_can_manage_uid(const struct task *task, uint32_t uid)
{
    return task && task->uid && uid &&
           (task_effective_role(task) == RELIEFOS_AUTH_ROLE_ADMIN ||
            task->uid == uid);
}

/**
 * Startup db find.
 * @param uid Identifier or flags controlling the operation.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_db_find(uint32_t uid, const struct reliefos_startup_command *command)
{
    for (uint32_t i = 0; i < startup_db_scratch.count; ++i) {
        if (startup_db_scratch.entries[i].uid == uid &&
            startup_command_equal(&startup_db_scratch.entries[i].entry.command, command)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Startup denial find.
 * @param uid Identifier or flags controlling the operation.
 * @param requester_path NUL-terminated text supplied by the caller.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_denial_find(uint32_t uid, const char *requester_path,
                               const struct reliefos_startup_command *command)
{
    for (uint32_t i = 0; i < startup_denial_db_scratch.count; ++i) {
        if (startup_denial_db_scratch.entries[i].uid == uid &&
            startup_text_eq(startup_denial_db_scratch.entries[i].requester_path, requester_path) &&
            startup_command_equal(&startup_denial_db_scratch.entries[i].command, command)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Startup remember denial.
 * @param uid Identifier or flags controlling the operation.
 * @param requester_path NUL-terminated text supplied by the caller.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_remember_denial(uint32_t uid, const char *requester_path,
                                   const struct reliefos_startup_command *command)
{
    if (startup_denial_find(uid, requester_path, command) >= 0 ||
        startup_denial_db_scratch.count >= STARTUP_DENIAL_MAX) {
        return -RELIEFOS_E2BIG;
    }
    uint32_t i = startup_denial_db_scratch.count++;
    startup_denial_db_scratch.entries[i].uid = uid;
    copy_text(startup_denial_db_scratch.entries[i].requester_path,
              sizeof(startup_denial_db_scratch.entries[i].requester_path), requester_path);
    startup_denial_db_scratch.entries[i].command = *command;
    return startup_denial_db_save();
}

/**
 * Startup request find.
 * @param id Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct startup_request_slot *startup_request_find(uint32_t id)
{
    for (uint32_t i = 0; i < STARTUP_REQUEST_MAX; ++i) {
        if (startup_requests[i].used && startup_requests[i].id == id) {
            return &startup_requests[i];
        }
    }
    return 0;
}

/**
 * Startup request reconcile.
 * @param slot Value supplied by the caller.
 */
static void startup_request_reconcile(struct startup_request_slot *slot)
{
    struct task *dialog;
    if (!slot || slot->status != RELIEFOS_STARTUP_STATUS_PENDING) {
        return;
    }
    dialog = sched_find(slot->dialog_pid);
    if (!dialog || dialog->state == TASK_EXITED) {
        slot->status = RELIEFOS_STARTUP_STATUS_DENIED;
    }
}

/**
 * Startup request alloc.
 * @return The value or status produced by the operation.
 */
static struct startup_request_slot *startup_request_alloc(void)
{
    for (uint32_t i = 0; i < STARTUP_REQUEST_MAX; ++i) {
        struct task *requester;
        startup_request_reconcile(&startup_requests[i]);
        requester = startup_requests[i].used
                        ? sched_find(startup_requests[i].requester_pid) : 0;
        if (startup_requests[i].used && startup_requests[i].status != RELIEFOS_STARTUP_STATUS_PENDING &&
            (!requester || requester->state == TASK_EXITED)) {
            startup_requests[i].used = 0;
        }
        if (!startup_requests[i].used) {
            startup_requests[i] = (struct startup_request_slot){0};
            startup_requests[i].used = 1;
            startup_requests[i].id = startup_next_request_id++;
            if (!startup_next_request_id) {
                startup_next_request_id = 1;
            }
            return &startup_requests[i];
        }
    }
    return 0;
}

/**
 * Startup add entry.
 * @param uid Identifier or flags controlling the operation.
 * @param command Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_add_entry(uint32_t uid, const struct reliefos_startup_command *command)
{
    int existing;
    uint32_t user_entry_count = 0;
    startup_db_load();
    existing = startup_db_find(uid, command);
    if (existing >= 0) {
        return 1;
    }
    for (uint32_t i = 0; i < startup_db_scratch.count; ++i) {
        if (startup_db_scratch.entries[i].uid == uid) {
            ++user_entry_count;
        }
    }
    if (user_entry_count >= RELIEFOS_STARTUP_MAX_ENTRIES) {
        return -RELIEFOS_E2BIG;
    }
    if (startup_db_scratch.count >= STARTUP_DB_ENTRY_MAX) {
        return -RELIEFOS_E2BIG;
    }
    uint32_t i = startup_db_scratch.count++;
    startup_db_scratch.entries[i].uid = uid;
    startup_db_scratch.entries[i].entry.id = startup_db_scratch.next_id++;
    if (!startup_db_scratch.next_id) {
        startup_db_scratch.next_id = 1;
    }
    startup_db_scratch.entries[i].entry.enabled = 1;
    startup_db_scratch.entries[i].entry.command = *command;
    return startup_db_save();
}

/**
 * Startup dialog spawn.
 * @param slot Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int startup_dialog_spawn(struct startup_request_slot *slot)
{
    const char *argv[] = {SYSCONFDIALOG_APP_PATH, 0};
    int64_t pid;
    if (!slot || !slot->user.uid || !slot->session_id) {
        return -RELIEFOS_EINVAL;
    }
    pid = userland_spawn_path_argv_for_user(SYSCONFDIALOG_APP_PATH, argv, 0,
                                            slot->requester_pid, &slot->user,
                                            slot->session_id);
    if (pid <= 0) {
        slot->status = RELIEFOS_STARTUP_STATUS_FAILED;
        return (int)pid;
    }
    slot->dialog_pid = (uint32_t)pid;
    return 0;
}
