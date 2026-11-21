/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include "interface.h"
#include "lz4/lz4.h"
#include "lzma/LzmaDec.h"
#include "xz/xz_config.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd/zstd.h"

#define SQFS_LZMA 2u
#define SQFS_LZO 3u
#define SQFS_XZ 4u
#define SQFS_LZ4 5u
#define SQFS_ZSTD 6u

int sqfs_codec_init(uint16_t algorithm, uint32_t block_size, const uint8_t *options,
                    uint16_t option_size, void **state)
{
    *state = NULL;
    switch (algorithm) {
    case SQFS_LZMA:
        return option_size ? -EINVAL : 0;
    case SQFS_LZO:
        /* The option record describes the encoder. All variants are LZO1X. */
        return !option_size || option_size == 8 ? 0 : -EINVAL;
    case SQFS_LZ4:
        /* SquashFS uses raw LZ4 blocks with the version-1 option record. */
        if (option_size != 8) return -EINVAL;
        return sqfs_u32(options) == 1 ? 0 : -EOPNOTSUPP;
    case SQFS_XZ: {
        if (option_size) {
            if (option_size != 8) return -EINVAL;
            uint32_t dict = sqfs_u32(options);
            uint32_t max = block_size > SQFS_META_BYTES ? block_size : SQFS_META_BYTES;
            if (dict < 4096 || dict > max) return -EINVAL;
            /* LZMA2 encodes a dictionary of 2^n or 3 * 2^n. */
            uint32_t normalized = dict >> __builtin_ctz(dict);
            if (normalized != 1 && normalized != 3) return -EINVAL;
        }
        /* Storage execution ownership also serializes initial table setup. */
        static bool crc_initialized;
        if (!crc_initialized) {
            xz_crc32_init();
            xz_crc64_init();
            crc_initialized = true;
        }
        /* In single-call mode the bounded output buffer is the dictionary. */
        *state = xz_dec_init(XZ_SINGLE, 0);
        return *state ? 0 : -ENOMEM;
    }
    case SQFS_ZSTD: {
        if (option_size && option_size != 4) return -EINVAL;
        size_t bytes = ZSTD_estimateDCtxSize();
        void *workspace = kernel_malloc(bytes);
        if (!workspace) return -ENOMEM;
        if (!ZSTD_initStaticDCtx(workspace, bytes)) {
            kernel_free(workspace);
            return -ENOMEM;
        }
        *state = workspace;
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}

void sqfs_codec_destroy(uint16_t algorithm, void *state)
{
    if (!state) return;
    if (algorithm == SQFS_XZ) xz_dec_end(state);
    else kernel_free(state);
}

static void *lzma_alloc(ISzAllocPtr allocator, size_t bytes)
{
    (void)allocator;
    return kernel_malloc(bytes);
}

static void lzma_free(ISzAllocPtr allocator, void *memory)
{
    (void)allocator;
    kernel_free(memory);
}

static int decode_lzma(const uint8_t *src, uint32_t stored, uint8_t *dst, uint32_t capacity,
                       uint32_t *size)
{
    static const ISzAlloc allocator = {lzma_alloc, lzma_free};
    enum { header = LZMA_PROPS_SIZE + 8 };
    if (stored <= header) return -EIO;
    uint64_t unpacked = sqfs_u64(src + LZMA_PROPS_SIZE);
    if (!unpacked || unpacked > capacity) return -EIO;
    size_t input_bytes = stored - header, output_bytes = unpacked;
    ELzmaStatus status;
    SRes result = LzmaDecode(dst, &output_bytes, src + header, &input_bytes, src,
                            LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &allocator);
    if (result == SZ_ERROR_MEM) return -ENOMEM;
    if (result != SZ_OK || output_bytes != unpacked || input_bytes != stored - header ||
        (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
        return -EIO;
    *size = output_bytes;
    return 0;
}

int sqfs_codec_decode(uint16_t algorithm, void *state, const uint8_t *src, uint32_t stored,
                      uint8_t *dst, uint32_t capacity, uint32_t *size)
{
    switch (algorithm) {
    case SQFS_LZMA:
        return decode_lzma(src, stored, dst, capacity, size);
    case SQFS_LZO:
        return sqfs_lzo_decode(src, stored, dst, capacity, size);
    case SQFS_XZ: {
        struct xz_buf block = {.in = src, .in_size = stored, .out = dst, .out_size = capacity};
        if (xz_dec_run(state, &block) != XZ_STREAM_END || block.in_pos != stored) return -EIO;
        *size = block.out_pos;
        return 0;
    }
    case SQFS_LZ4: {
        int decoded = LZ4_decompress_safe((const char *)src, (char *)dst, stored, capacity);
        if (decoded < 0) return -EIO;
        *size = decoded;
        return 0;
    }
    case SQFS_ZSTD: {
        if (ZSTD_findFrameCompressedSize(src, stored) != stored) return -EIO;
        size_t decoded = ZSTD_decompressDCtx(state, dst, capacity, src, stored);
        if (ZSTD_isError(decoded)) return -EIO;
        *size = decoded;
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}
