/* ext4 metadata checksums: table-driven CRC32C/CRC16 primitives plus the
 * superblock, group-descriptor and inode checksum algorithms of
 * linux/fs/ext4/{super.c,inode.c} and linux/lib/crc/crc16.c (Linux v7.3-rc5
 * reference tree).  Host-testable pure functions: no kernel services, no
 * dynamic allocation, no global state.  Every routine only reads or writes
 * the caller-provided buffer; verify() never writes and update() only stamps
 * the checksum field of the buffer it was given.
 *
 * Return codes for the verify/update entry points:
 *   0               checksum matches (verify) / written (update); also
 *                   returned when the relevant checksum feature is off,
 *                   in which case update() leaves the buffer untouched
 *   -RELIEFOS_EIO   checksum mismatch
 *   -RELIEFOS_EINVAL  NULL pointer or out-of-range length/size argument
 */
#include "../block/storage_internal.h"

/* On-disk offsets (linux/fs/ext4/ext4.h:1369/402/231). */
#define CS_SB_FEATURE_RO_COMPAT 0x64
#define CS_SB_CHECKSUM 0x3FC
#define CS_GD_CHECKSUM 0x1E
#define CS_GD_TAIL 0x20
#define CS_INO_CHECKSUM_LO 0x7C
#define CS_INO_EXTRA_ISIZE 0x80
#define CS_INO_CHECKSUM_HI 0x82
#define CS_INO_OLD 0x80
#define CS_INO_HI_END 0x84
/* EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi) boils down to
 * offsetof(i_checksum_hi) + 2 - EXT4_GOOD_OLD_INODE_SIZE == 4 <=
 * i_extra_isize (ext4.h:869-873). */
#define CS_INO_HI_EXTRA_END 4u

/* CRC-32C (Castagnoli), reflected form 0x82F63B78.  Built once as a 256
 * entry table (never generated at run time); chained exactly like
 * linux's ext4_chksum(): caller seed, no final xor. */
static const uint32_t crc32c_table[256] = {
    0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u,
    0xc79a971fu, 0x35f1141cu, 0x26a1e7e8u, 0xd4ca64ebu,
    0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu,
    0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u,
    0x105ec76fu, 0xe235446cu, 0xf165b798u, 0x030e349bu,
    0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
    0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u,
    0x5d1d08bfu, 0xaf768bbcu, 0xbc267848u, 0x4e4dfb4bu,
    0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au,
    0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u,
    0xaa64d611u, 0x580f5512u, 0x4b5fa6e6u, 0xb93425e5u,
    0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
    0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u,
    0xf779deaeu, 0x05125dadu, 0x1642ae59u, 0xe4292d5au,
    0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au,
    0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u,
    0x417b1dbcu, 0xb3109ebfu, 0xa0406d4bu, 0x522bee48u,
    0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
    0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u,
    0x0c38d26cu, 0xfe53516fu, 0xed03a29bu, 0x1f682198u,
    0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u,
    0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u,
    0xdbfc821cu, 0x2997011fu, 0x3ac7f2ebu, 0xc8ac71e8u,
    0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
    0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u,
    0xa65c047du, 0x5437877eu, 0x4767748au, 0xb50cf789u,
    0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u,
    0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u,
    0x7198540du, 0x83f3d70eu, 0x90a324fau, 0x62c8a7f9u,
    0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
    0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u,
    0x3cdb9bddu, 0xceb018deu, 0xdde0eb2au, 0x2f8b6829u,
    0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu,
    0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u,
    0x082f63b7u, 0xfa44e0b4u, 0xe9141340u, 0x1b7f9043u,
    0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
    0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u,
    0x55326b08u, 0xa759e80bu, 0xb4091bffu, 0x466298fcu,
    0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu,
    0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u,
    0xa24bb5a6u, 0x502036a5u, 0x4370c551u, 0xb11b4652u,
    0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
    0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du,
    0xef087a76u, 0x1d63f975u, 0x0e330a81u, 0xfc588982u,
    0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du,
    0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u,
    0x38cc2a06u, 0xcaa7a905u, 0xd9f75af1u, 0x2b9cd9f2u,
    0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
    0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u,
    0x0417b1dbu, 0xf67c32d8u, 0xe52cc12cu, 0x1747422fu,
    0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu,
    0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u,
    0xd3d3e1abu, 0x21b862a8u, 0x32e8915cu, 0xc083125fu,
    0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
    0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u,
    0x9e902e7bu, 0x6cfbad78u, 0x7fab5e8cu, 0x8dc0dd8fu,
    0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu,
    0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u,
    0x69e9f0d5u, 0x9b8273d6u, 0x88d28022u, 0x7ab90321u,
    0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
    0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u,
    0x34f4f86au, 0xc69f7b69u, 0xd5cf889du, 0x27a40b9eu,
    0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu,
    0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u,
};

/* CRC-16, poly 0x8005 (reflected table form 0xA001), identical to the
 * table in linux/lib/crc/crc16.c; caller seed, no final xor. */
static const uint16_t crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,};

/* Local little-endian accessors: this module must stay linkable on its own
 * (the host test builds it without storage_ext4_format.c), so it does not
 * reuse the ext4_get/put_le* helpers defined there. */
static uint16_t csum_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t csum_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void csum_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)(value >> 8);
}

static void csum_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

uint32_t storage_ext4_crc32c(uint32_t seed, const uint8_t *data, uint32_t len)
{
    uint32_t crc = seed;

    if (len == 0)
        return seed;
    if (data == NULL)
        return 0;
    while (len-- > 0)
        crc = (crc >> 8) ^ crc32c_table[(crc ^ *data++) & 0xffu];
    return crc;
}

uint16_t storage_ext4_crc16(uint16_t seed, const uint8_t *data, uint32_t len)
{
    uint16_t crc = seed;

    if (len == 0)
        return seed;
    if (data == NULL)
        return 0;
    while (len-- > 0)
        crc = (uint16_t)((crc >> 8) ^ crc16_table[(crc ^ *data++) & 0xffu]);
    return crc;
}

uint32_t storage_ext4_super_csum_seed(const struct storage_ext4_super_view *view)
{
    if (view == NULL)
        return 0;
    /* s_csum_seed selection (super.c:4722-4726). */
    if ((view->feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) != 0)
        return view->checksum_seed;
    if ((view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0 ||
        (view->feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) != 0)
        return storage_ext4_crc32c(~0u, view->uuid, 16);
    return 0;
}

/* The superblock checksum covers every byte before s_checksum and uses the
 * fixed seed ~0 (super.c:302-311), never s_csum_seed. */
static uint32_t super_checksum(const uint8_t *sb_raw)
{
    return storage_ext4_crc32c(~0u, sb_raw, CS_SB_CHECKSUM);
}

int storage_ext4_verify_super_checksum(const uint8_t *sb_raw, uint32_t sb_len)
{
    if (sb_raw == NULL || sb_len < EXT4_SUPERBLOCK_SIZE)
        return -RELIEFOS_EINVAL;
    if ((csum_get_le32(sb_raw + CS_SB_FEATURE_RO_COMPAT) &
         EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0)
        return 0;
    if (csum_get_le32(sb_raw + CS_SB_CHECKSUM) != super_checksum(sb_raw))
        return -RELIEFOS_EIO;
    return 0;
}

int storage_ext4_update_super_checksum(uint8_t *sb_raw, uint32_t sb_len)
{
    if (sb_raw == NULL || sb_len < EXT4_SUPERBLOCK_SIZE)
        return -RELIEFOS_EINVAL;
    if ((csum_get_le32(sb_raw + CS_SB_FEATURE_RO_COMPAT) &
         EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0)
        return 0;
    csum_put_le32(sb_raw + CS_SB_CHECKSUM, super_checksum(sb_raw));
    return 0;
}

/* Group descriptor checksum (super.c:3262-3300).  The bg_checksum field is
 * hashed as two zero bytes on the metadata_csum path and skipped on the
 * legacy gdt_csum path, matching Linux exactly; neither path reads the
 * stored checksum while computing. */
static uint16_t group_checksum(const uint8_t *gd_raw, uint32_t desc_size,
                               uint32_t group,
                               const struct storage_ext4_super_view *view)
{
    uint8_t le_group[4];
    static const uint8_t zeros2[2];

    csum_put_le32(le_group, group);
    if ((view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0) {
        uint32_t csum32 = storage_ext4_super_csum_seed(view);

        csum32 = storage_ext4_crc32c(csum32, le_group, 4);
        csum32 = storage_ext4_crc32c(csum32, gd_raw, CS_GD_CHECKSUM);
        csum32 = storage_ext4_crc32c(csum32, zeros2, 2);
        if (desc_size > CS_GD_TAIL)
            csum32 = storage_ext4_crc32c(csum32, gd_raw + CS_GD_TAIL,
                                         desc_size - CS_GD_TAIL);
        return (uint16_t)(csum32 & 0xffffu);
    }

    /* Legacy crc16 chain (gdt_csum / uninit_bg).  The tail past the
     * checksum slot only participates with INCOMPAT_64BIT
     * (super.c:3293-3295). */
    {
        uint16_t crc = storage_ext4_crc16(0xffffu, view->uuid, 16);

        crc = storage_ext4_crc16(crc, le_group, 4);
        crc = storage_ext4_crc16(crc, gd_raw, CS_GD_CHECKSUM);
        if ((view->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0 &&
            desc_size > CS_GD_TAIL)
            crc = storage_ext4_crc16(crc, gd_raw + CS_GD_TAIL,
                                     desc_size - CS_GD_TAIL);
        return crc;
    }
}

static int group_args_ok(const uint8_t *gd_raw, uint32_t desc_size,
                         const struct storage_ext4_super_view *view)
{
    if (gd_raw == NULL || view == NULL)
        return 0;
    if (desc_size < EXT4_MIN_DESC_SIZE || desc_size > EXT4_MAX_DESC_SIZE ||
        (desc_size & 3u) != 0)
        return 0;
    return 1;
}

static int group_csum_active(const struct storage_ext4_super_view *view)
{
    return (view->feature_ro_compat &
            (EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |
             EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) != 0;
}

int storage_ext4_verify_group_checksum(const uint8_t *gd_raw, uint32_t desc_size,
                                       uint32_t group,
                                       const struct storage_ext4_super_view *view)
{
    if (!group_args_ok(gd_raw, desc_size, view))
        return -RELIEFOS_EINVAL;
    if (!group_csum_active(view))
        return 0;
    if (csum_get_le16(gd_raw + CS_GD_CHECKSUM) !=
        group_checksum(gd_raw, desc_size, group, view))
        return -RELIEFOS_EIO;
    return 0;
}

int storage_ext4_update_group_checksum(uint8_t *gd_raw, uint32_t desc_size,
                                       uint32_t group,
                                       const struct storage_ext4_super_view *view)
{
    if (!group_args_ok(gd_raw, desc_size, view))
        return -RELIEFOS_EINVAL;
    if (!group_csum_active(view))
        return 0;
    csum_put_le16(gd_raw + CS_GD_CHECKSUM,
                  group_checksum(gd_raw, desc_size, group, view));
    return 0;
}

/* EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi): the i_checksum_hi slot at
 * 0x82 exists only when i_extra_isize >= 4 (ext4.h:869-873).  Must not be
 * consulted for good-old 128-byte inodes (the buffer ends at 0x80). */
static int inode_csum_hi_fits(const uint8_t *ino_raw, uint32_t inode_size)
{
    return inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
           csum_get_le16(ino_raw + CS_INO_EXTRA_ISIZE) >= CS_INO_HI_EXTRA_END;
}

/* Inode checksum (inode.c:58-124) with the per-inode seed of
 * inode.c:5404-5411.  i_checksum_lo at 0x7C hashes as two zero bytes.
 * Past the good-old size, i_checksum_hi at 0x82 hashes as two zero bytes
 * only where the field exists (EXT4_FITS_IN_INODE, see above); otherwise
 * 0x82 onward is ordinary hash input and the result is truncated to the
 * low 16 bits on disk.  Linux gates this on s_creator_os ==
 * EXT4_OS_LINUX as well; that field is not observable through
 * storage_ext4_super_view, so callers must only checksum inodes of
 * Linux-created (creator_os 0) filesystems. */
static uint32_t inode_checksum(const uint8_t *ino_raw, uint32_t inode_size,
                               uint32_t ino, uint32_t generation,
                               const struct storage_ext4_super_view *view)
{
    uint8_t le_ino[4], le_gen[4];
    static const uint8_t zeros2[2];
    uint32_t csum;

    csum_put_le32(le_ino, ino);
    csum_put_le32(le_gen, generation);
    csum = storage_ext4_super_csum_seed(view);
    csum = storage_ext4_crc32c(csum, le_ino, 4);
    csum = storage_ext4_crc32c(csum, le_gen, 4);
    csum = storage_ext4_crc32c(csum, ino_raw, CS_INO_CHECKSUM_LO);
    csum = storage_ext4_crc32c(csum, zeros2, 2);
    csum = storage_ext4_crc32c(csum, ino_raw + CS_INO_CHECKSUM_LO + 2,
                               CS_INO_OLD - CS_INO_CHECKSUM_LO - 2);
    if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        csum = storage_ext4_crc32c(csum, ino_raw + CS_INO_EXTRA_ISIZE,
                                   CS_INO_CHECKSUM_HI - CS_INO_EXTRA_ISIZE);
        if (inode_csum_hi_fits(ino_raw, inode_size)) {
            csum = storage_ext4_crc32c(csum, zeros2, 2);
            csum = storage_ext4_crc32c(csum, ino_raw + CS_INO_HI_END,
                                       inode_size - CS_INO_HI_END);
        } else {
            csum = storage_ext4_crc32c(csum, ino_raw + CS_INO_CHECKSUM_HI,
                                       inode_size - CS_INO_CHECKSUM_HI);
        }
    }
    return csum;
}

static int inode_args_ok(const uint8_t *ino_raw, uint32_t inode_size,
                         const struct storage_ext4_super_view *view)
{
    if (ino_raw == NULL || view == NULL)
        return 0;
    if (inode_size < EXT4_GOOD_OLD_INODE_SIZE ||
        inode_size > EXT4_MAX_BLOCK_SIZE ||
        (inode_size % EXT4_GOOD_OLD_INODE_SIZE) != 0)
        return 0;
    return 1;
}

int storage_ext4_verify_inode_checksum(const uint8_t *ino_raw,
                                       uint32_t inode_size, uint32_t ino,
                                       uint32_t generation,
                                       const struct storage_ext4_super_view *view)
{
    uint32_t provided, calculated;

    if (!inode_args_ok(ino_raw, inode_size, view))
        return -RELIEFOS_EINVAL;
    if ((view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0)
        return 0;
    calculated = inode_checksum(ino_raw, inode_size, ino, generation, view);
    provided = csum_get_le16(ino_raw + CS_INO_CHECKSUM_LO);
    if (inode_csum_hi_fits(ino_raw, inode_size))
        provided |= (uint32_t)csum_get_le16(ino_raw + CS_INO_CHECKSUM_HI) << 16;
    else
        calculated &= 0xffffu;
    return provided == calculated ? 0 : -RELIEFOS_EIO;
}

int storage_ext4_update_inode_checksum(uint8_t *ino_raw, uint32_t inode_size,
                                       uint32_t ino, uint32_t generation,
                                       const struct storage_ext4_super_view *view)
{
    uint32_t csum;

    if (!inode_args_ok(ino_raw, inode_size, view))
        return -RELIEFOS_EINVAL;
    if ((view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0)
        return 0;
    csum = inode_checksum(ino_raw, inode_size, ino, generation, view);
    csum_put_le16(ino_raw + CS_INO_CHECKSUM_LO, (uint16_t)(csum & 0xffffu));
    if (inode_csum_hi_fits(ino_raw, inode_size))
        csum_put_le16(ino_raw + CS_INO_CHECKSUM_HI, (uint16_t)(csum >> 16));
    return 0;
}
