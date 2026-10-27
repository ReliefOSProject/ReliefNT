/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PARAMETER_INTERNAL_H
#define RELIEFNT_PARAMETER_INTERNAL_H
#include <reliefnt/moduleparam.h>
struct parameter_string;
struct parameter_values {
    const struct module_parameter *records;
    size_t count;
    struct parameter_string *strings;
};
/** @brief Determine the storage width of a supported parameter type.
 * @param parameter Stable descriptor retained by the caller.
 * @return Byte width, or zero for an unknown type. No locks or allocation.
 */
size_t parameter_value_width(const struct module_parameter *parameter);
/** @brief Match Linux parameter names with dash/underscore equivalence.
 * @param left Borrowed terminated input.
 * @param right Borrowed terminated descriptor name.
 * @return Equality; no references or locks acquired.
 */
bool parameter_name_equal(const char *left,const char *right);
/** @brief Set a scalar or transactionally replace a framework-owned string.
 * @param values Exclusively owned parameter table retaining string allocations.
 * @param parameter Borrowed validated descriptor in that table.
 * @param value Terminated input, NULL only for a bare boolean.
 * @return Zero or negative errno, preserving the old value on error. Task
 * context under execution ownership; no registry lock. Never frees a module's
 * initial literal. String consumers must serialize with runtime writes.
 */
int parameter_value_set(struct parameter_values *values,const struct module_parameter *parameter,const char *value);
/** @brief Format a parameter as a newline-terminated sysfs byte stream.
 * @param parameter Validated descriptor with retained backing storage.
 * @param offset Byte position, including EOF and beyond.
 * @param buffer Caller-owned output, nullable only for zero capacity.
 * @param capacity Maximum output bytes.
 * @param written Required result, cleared on entry.
 * @return Zero or negative errno. Execution ownership serializes string
 * replacement; does not allocate, acquire locks or transfer references.
 */
int parameter_value_read(const struct module_parameter *parameter,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written);
/** @brief Free framework-owned strings after module exit and reference drain.
 * @param values Exclusively owned table, nullable; list becomes empty.
 * Task context without registry locks. Initial module literals are not freed.
 */
void parameter_values_release(struct parameter_values *values);
/** @brief Parse an owned copy of Linux-style quoted parameter tokens.
 * @param values Exclusively owned destination descriptors/string ledger.
 * @param arguments Borrowed terminated input, nullable.
 * @param ignore_unknown True for builtin boot options handled by other parsers.
 * @return Zero or negative errno. Boot or execution-owned task context;
 * temporary allocation is freed on every return, with no locks held.
 */
int parameter_arguments(struct parameter_values *values,const char *arguments,bool ignore_unknown);
/** @brief Find permanent builtin records owned by a module name.
 * @param name Terminated module name.
 * @return Borrowed first descriptor or NULL; never allocates or pins code.
 */
const struct module_parameter *parameter_builtin_first(const char *name);
/** @brief Retrieve the permanent builtin parameter table and owned strings.
 * @return Permanent execution-owned table; no ownership transfer or locks.
 */
struct parameter_values *parameter_builtin_values(void);
#endif
