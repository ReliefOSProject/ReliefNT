/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_SIDECAR_H
#define RELIEFNT_FS_SIDECAR_H
#include <reliefnt/storage.h>

/* Both callbacks use one already selected directory under the execution
 * transaction. read returns a newly owned mm_alloc_pages buffer and its size,
 * or -ENOENT with no allocation. Errors transfer nothing. write stores every
 * byte or returns negative errno. Neither callback changes namespace roots. */
struct storage_sidecar_io {
    void *context;
    int (*read)(void *context,const void **data,size_t *length);
    int (*write)(void *context,const void *data,uint32_t length);
};
/** @brief Read or store FAT-family metadata in an already bound directory.
 * @param io Borrowed selected-directory I/O and context, valid for this call.
 * @param name Single component, or dot for the filesystem root's own record.
 * @param intrinsic Backend-root-relative absolute name used only for defaults;
 * mount moves and overmounts must not change this classification.
 * @param value Read output or borrowed metadata to store.
 * @param write True to update, false to query; read-only I/O may omit write.
 * @return Zero or negative errno, including EIO for corrupt metadata. Caller
 * serializes directory mutation; temporary page buffers are always released.
 * No global namespace resolution, module or graph lock occurs in the codec.
 */
int storage_sidecar_permissions_at(const struct storage_sidecar_io *io,const char *name,
                                      const char *intrinsic,struct reliefos_permissions *value,bool write);
/* Both images own page buffers of exactly length bytes, released together. */
struct storage_sidecar_change {void *before,*after;uint32_t length;};
/** @brief Build both rename metadata images before any namespace mutation.
 * @param io Borrowed selected-directory reader; never written by this helper.
 * @param old_name Source component, case-insensitive in the existing codec.
 * @param new_name Destination component replacing its previous record.
 * @param fallback Source POSIX metadata when no explicit old record exists.
 * @param out Empty owned images, including valid unknown TLVs reserving space.
 * @return Zero or negative errno; all allocation/validation precedes mutation.
 * Existing ACL records are preserved. Caller releases images on every outcome.
 */
int storage_sidecar_prepare_rename(const struct storage_sidecar_io *io,const char *old_name,
                                      const char *new_name,const struct reliefos_permissions *fallback,
                                      struct storage_sidecar_change *out);
/** @brief Prepare record removal and full-ACL insertion in distinct directories.
 * @param source Borrowed selected source-directory reader.
 * @param old_name Existing source component.
 * @param destination Borrowed different destination-directory reader.
 * @param new_name Destination component replacing any previous record.
 * @param fallback Source POSIX metadata when no explicit source record exists.
 * @param old_image Empty owned source before/after images on success.
 * @param new_image Empty owned destination before/after images on success.
 * @return Zero or negative errno; neither directory is written. All partial
 * page buffers are released on failure. Caller serializes both directories.
 */
int storage_sidecar_prepare_move(const struct storage_sidecar_io *source,const char *old_name,
                                    const struct storage_sidecar_io *destination,const char *new_name,
                                    const struct reliefos_permissions *fallback,
                                    struct storage_sidecar_change *old_image,struct storage_sidecar_change *new_image);
/** @brief Release both prepared metadata images without filesystem I/O.
 * @param change Owned images, nullable; cleared so repeated release is harmless.
 */
void storage_sidecar_change_release(struct storage_sidecar_change *change);
#endif
