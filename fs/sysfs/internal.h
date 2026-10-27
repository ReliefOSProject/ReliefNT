/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SYSFS_INTERNAL_H
#define RELIEFNT_SYSFS_INTERNAL_H
#include <reliefnt/moduleparam.h>
#include <reliefnt/permissions.h>
/** @brief Decode an intrinsic sysfs module parameter key.
 * @param path Borrowed provider-internal path, never a global routing input.
 * @param module Required MODULE_NAME_LEN-byte module output.
 * @param name Receives a borrowed local name within path.
 * @return True for one syntactically valid parameter key, false otherwise.
 * No locks, allocation or ownership transfer; caller retains the input.
 */
bool sysfs_parameter_key(const char *path,char module[MODULE_NAME_LEN],const char **name);
/** @brief Store a parameter through its retained generated inode.
 * @param path Borrowed intrinsic key whose identity was checked by provider.
 * @param offset Byte position, sysfs stores treat each write as a whole value.
 * @param buffer Validated payload.
 * @param length Payload bytes.
 * @param written Required output, zero on failure.
 * @return Zero or negative errno. Execution ownership required; registry
 * pins are balanced and no locks span string allocation.
 */
int sysfs_parameter_write(const char *path,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written);
/** @brief Accept O_TRUNC's zero length only for an existing writable attribute.
 * @param path Borrowed retained intrinsic key.
 * @param length Requested size.
 * @return Zero or negative errno. Execution-owned, no allocation or mutation;
 * actual parameter changes happen only after a successful write.
 */
int sysfs_parameter_truncate(const char *path,uint64_t length);
/** @brief Read descriptor modes while retaining normal inventory permissions.
 * @param path Intrinsic generated key.
 * @param node Retained generated inode.
 * @param value Required metadata output.
 * @param write Whether metadata itself is being changed.
 * @return Zero or negative errno; metadata changes remain unsupported.
 * Execution ownership required; no references escape the function.
 */
int sysfs_parameter_permissions(const char *path,const struct storage_node *node,struct reliefos_permissions *value,bool write);
#endif
