/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULEPARAM_H
#define RELIEFNT_MODULEPARAM_H

#include <reliefnt/module.h>

enum module_parameter_type {
    MODULE_PARAMETER_bool,
    MODULE_PARAMETER_int,
    MODULE_PARAMETER_uint,
    MODULE_PARAMETER_long,
    MODULE_PARAMETER_ulong,
    MODULE_PARAMETER_charp
};

/* Writable strings belong to the parameter framework, not module exit code. */
struct module_parameter {
    const char *name;
    void *value;
    enum module_parameter_type type;
    unsigned int permissions;
};

#ifdef MODULE
#define __RN_PARAM_PREFIX ""
#else
#define __RN_PARAM_PREFIX KBUILD_MODNAME "."
#endif
#define __RN_PARAM(name, variable, kind, type_name, permissions) \
    static const struct module_parameter __RN_JOIN(__module_param_, name) \
    __attribute__((used, section("__param"), aligned(8))) = \
        { __RN_PARAM_PREFIX #name, &(variable), kind, permissions }; \
    MODULE_INFO(parmtype, #name ":" type_name)
#define module_param_named(name, variable, type, permissions) \
    __RN_PARAM(name, variable, MODULE_PARAMETER_##type, #type, permissions)
#define module_param(name, type, permissions) \
    __RN_PARAM(name, name, MODULE_PARAMETER_##type, #type, permissions)
#define MODULE_PARM_DESC(name, description) MODULE_INFO(parm, #name ":" description)

#define MODULE_PARAMETER_NAME_LEN 256u
struct module_parameter_info {
    char name[MODULE_PARAMETER_NAME_LEN];
    unsigned int permissions;
};
/** @brief Enumerate visible attributes of an actual builtin or LIVE module.
 * @param module Borrowed terminated module name.
 * @param index Zero-based visible parameter index.
 * @param out Caller-owned descriptor snapshot with no runtime pointers.
 * @return Zero or negative errno. Execution ownership required; dynamic code
 * is pinned only while copying, with no allocation under the registry lock.
 */
int module_parameter_at(const char *module,size_t index,struct module_parameter_info *out);
/** @brief Read one visible parameter while its exact runtime remains pinned.
 * @param module Borrowed terminated module name.
 * @param name Borrowed terminated parameter name.
 * @param offset Stream byte offset.
 * @param buffer Writable output, NULL only for zero capacity.
 * @param capacity Maximum output bytes.
 * @param written Required actual byte count, cleared on error.
 * @return Zero or negative errno; execution ownership serializes setters and
 * readers. Does not hold the module registry lock across formatting.
 */
int module_parameter_read(const char *module,const char *name,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written);
/** @brief Set a writable parameter with normal sysfs string semantics.
 * @param module Borrowed module name.
 * @param name Borrowed parameter name.
 * @param buffer Complete write payload, not necessarily NUL terminated.
 * @param length Payload byte count.
 * @param written Required result, full length on success, zero on error.
 * @return Zero or negative errno. Task/execution ownership required; a LIVE
 * image pin spans lookup/allocation/store and is balanced on all returns.
 * Permission-zero and readonly descriptors cannot be written even by root.
 */
int module_parameter_write(const char *module,const char *name,const void *buffer,uint32_t length,uint32_t *written);
/** @brief List builtin modules with at least one visible parameter.
 * @param index Zero-based distinct module index in linker table order.
 * @param out Writable MODULE_NAME_LEN-byte name.
 * @return Zero or ENOENT/EINVAL. No allocation, locks or ownership transfer.
 */
int module_parameter_builtin_at(size_t index,char out[MODULE_NAME_LEN]);
/** @brief Test a permanent builtin parameter namespace.
 * @param name Borrowed terminated module name.
 * @return Whether any builtin descriptor belongs to it, including invisible
 * attributes. No allocation, synchronization or ownership transfer.
 */
bool module_parameter_builtin_exists(const char *name);
/** @brief Apply fully qualified builtin parameters before driver initcalls.
 * @param arguments Borrowed terminated boot command line, nullable.
 * @return Zero or negative parse/allocation errno. Boot task context after
 * heap initialization and before SMP driver access. Unknown options are left
 * to their own boot parsers; quoted values and dash equivalence are accepted.
 */
int module_parameters_boot(const char *arguments);

#endif
