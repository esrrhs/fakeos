package fs;
import drivers;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Minimal Ext2 (rev 0/1) over VirtIO-blk.
 *
 * Mount, path lookup, open/read/write/lseek, create, mkdir, getdents,
 * unlink (regular files), sync. Block size 1024/2048/4096. Multi block
 * group (BGDT indexed). Direct + single-/double-/triple-indirect maps.
 * Hardening: free-count bookkeeping in SB/BGDT, s_state dirty-on-mount /
 * clean-on-sync (not a full journal).
 *
 * On-disk fields are accessed at explicit byte offsets (no packed structs)
 * so fakecc layout cannot introduce padding bugs.
 * ------------------------------------------------------------------------- */

enum {
    EXT2_MAGIC      = 0xEF53,
    EXT2_ROOT_INO   = 2,
    EXT2_S_IFDIR    = 0x4000,
    EXT2_S_IFREG    = 0x8000,
    EXT2_S_IFMT     = 0xF000,
    EXT2_FT_REG     = 1,
    EXT2_FT_DIR     = 2,

    SECTOR_SIZE     = 512,
    NAME_MAX        = 255,

    MAX_OPEN        = 32,
    FH_BASE         = 0x80000000u,

    O_RDONLY        = 0,
    O_WRONLY        = 1,
    O_RDWR          = 2,
    O_CREAT         = 0x040,
    O_TRUNC         = 0x200,
    O_APPEND        = 0x400,

    SEEK_SET        = 0,
    SEEK_CUR        = 1,
    SEEK_END        = 2,

    DENT_RECLEN     = 48,

    I_MODE          = 0,
    I_SIZE_LO       = 4,
    I_LINKS         = 26,
    I_BLOCKS        = 28,
    I_BLOCK         = 40
};

struct ext2_sb {
    u32 inodes_count;
    u32 blocks_count;
    u32 first_data_block;
    u32 log_block_size;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 magic;
    u32 inode_size;
    u32 first_ino;
    u32 block_size;
    u32 bg_block;
    u32 groups_count;
    u32 free_blocks;
    u32 free_inodes;
    u32 mounted;
};

/* Cached descriptor for one block group (32-byte on-disk BGD). */
struct ext2_bg {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u32 free_blocks;
    u32 free_inodes;
};

struct ext2_open {
    u32 used;
    u32 refs;
    u32 ino;
    u32 mode;
    u32 readable;
    u32 writable;
    u32 append;
    u64 off;
    u64 size;
};

static struct ext2_sb sb;
static struct ext2_open opens[MAX_OPEN];
static u8 blk_buf[4096];
static u8 ind_buf[4096];
static u8 tind_buf[4096];
static u8 ino_raw[128];

static u16 rd16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8)
         | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}
static void wr32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}
static u32 name_eq(const char *a, u32 an, const char *b, u32 bn) {
    u32 i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static u32 bread(u32 block, u8 *dst) {
    u32 sectors = sb.block_size / SECTOR_SIZE;
    u64 sector0 = ((u64)block * (u64)sb.block_size) / (u64)SECTOR_SIZE;
    return drivers.blk_read(sector0, dst, sectors);
}
static u32 bwrite(u32 block, const u8 *src) {
    u32 sectors = sb.block_size / SECTOR_SIZE;
    u64 sector0 = ((u64)block * (u64)sb.block_size) / (u64)SECTOR_SIZE;
    return drivers.blk_write(sector0, src, sectors);
}

static u32 bitmap_test(u8 *bm, u32 bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}
static void bitmap_set(u8 *bm, u32 bit) {
    bm[bit / 8] = (u8)(bm[bit / 8] | (u8)(1u << (bit % 8)));
}
static void bitmap_clr(u8 *bm, u32 bit) {
    bm[bit / 8] = (u8)(bm[bit / 8] & (u8)(~(1u << (bit % 8))));
}

/* Load block-group descriptor `g` from the BGDT. */
static u32 bg_load(u32 g, struct ext2_bg *out) {
    u32 descs_per = sb.block_size / 32;
    u32 blk;
    u32 off;
    if (g >= sb.groups_count) return 0;
    blk = sb.bg_block + g / descs_per;
    off = (g % descs_per) * 32;
    if (!bread(blk, blk_buf)) return 0;
    out->block_bitmap = rd32(blk_buf + off + 0);
    out->inode_bitmap = rd32(blk_buf + off + 4);
    out->inode_table = rd32(blk_buf + off + 8);
    out->free_blocks = rd16(blk_buf + off + 12);
    out->free_inodes = rd16(blk_buf + off + 14);
    return 1;
}

static u32 bg_store(u32 g, const struct ext2_bg *in) {
    u32 descs_per = sb.block_size / 32;
    u32 blk;
    u32 off;
    if (g >= sb.groups_count) return 0;
    blk = sb.bg_block + g / descs_per;
    off = (g % descs_per) * 32;
    if (!bread(blk, blk_buf)) return 0;
    wr32(blk_buf + off + 0, in->block_bitmap);
    wr32(blk_buf + off + 4, in->inode_bitmap);
    wr32(blk_buf + off + 8, in->inode_table);
    wr16(blk_buf + off + 12, (u16)in->free_blocks);
    wr16(blk_buf + off + 14, (u16)in->free_inodes);
    return bwrite(blk, blk_buf);
}

/* Superblock at byte 1024: update free counts + optional state. */
static u32 sb_write_counts(u16 state) {
    u8 sbsec[1024];
    if (!drivers.blk_read(2, sbsec, 2)) return 0;
    wr32(sbsec + 12, sb.free_blocks);
    wr32(sbsec + 16, sb.free_inodes);
    if (state != 0xFFFF) {
        wr16(sbsec + 58, state);
    }
    return drivers.blk_write(2, sbsec, 2);
}

static u32 group_of_block(u32 blk) {
    if (blk < sb.first_data_block) return 0;
    return (blk - sb.first_data_block) / sb.blocks_per_group;
}

static u32 group_of_inode(u32 ino) {
    if (ino == 0) return 0;
    return (ino - 1) / sb.inodes_per_group;
}

static u32 alloc_block(void) {
    u32 g;
    u32 z;
    for (g = 0; g < sb.groups_count; g++) {
        struct ext2_bg bg;
        u32 base;
        u32 lim;
        u32 i;
        u32 local;
        if (!bg_load(g, &bg)) continue;
        /* free_blocks is advisory; still scan the bitmap. */
        base = sb.first_data_block + g * sb.blocks_per_group;
        lim = base + sb.blocks_per_group;
        if (lim > sb.blocks_count) lim = sb.blocks_count;
        if (!bread(bg.block_bitmap, blk_buf)) continue;
        for (i = (g == 0 ? sb.first_data_block : base); i < lim; i++) {
            local = i - base;
            /* Group 0 bitmap historically indexed by absolute block number
             * for 1K single-group images (bit N = block N). Multi-group
             * images use group-relative bits. Detect via blocks_per_group. */
            if (sb.groups_count == 1) {
                local = i;
            }
            /* Never hand out SB/BGDT/bitmap/itable blocks even if a bit
             * was wrongly cleared (multi-bg relative bit0 == SB). */
            {
                u32 it_blocks = (sb.inodes_per_group * sb.inode_size + sb.block_size - 1)
                                / sb.block_size;
                u32 meta = 0;
                if (i <= sb.bg_block) meta = 1;
                if (i == bg.block_bitmap || i == bg.inode_bitmap) meta = 1;
                if (i >= bg.inode_table && i < bg.inode_table + it_blocks) meta = 1;
                if (meta) {
                    if (!bitmap_test(blk_buf, local)) {
                        bitmap_set(blk_buf, local);
                        bwrite(bg.block_bitmap, blk_buf);
                    }
                    continue;
                }
            }
            if (!bitmap_test(blk_buf, local)) {
                bitmap_set(blk_buf, local);
                if (!bwrite(bg.block_bitmap, blk_buf)) return 0;
                for (z = 0; z < sb.block_size; z++) blk_buf[z] = 0;
                if (!bwrite(i, blk_buf)) return 0;
                if (bg.free_blocks > 0) bg.free_blocks--;
                bg_store(g, &bg);
                if (sb.free_blocks > 0) sb.free_blocks--;
                sb_write_counts(0xFFFF);
                return i;
            }
        }
    }
    return 0;
}

static u32 alloc_inode_num(void) {
    u32 g;
    for (g = 0; g < sb.groups_count; g++) {
        struct ext2_bg bg;
        u32 start;
        u32 end;
        u32 i;
        if (!bg_load(g, &bg)) continue;
        start = g * sb.inodes_per_group + 1;
        end = start + sb.inodes_per_group - 1;
        if (end > sb.inodes_count) end = sb.inodes_count;
        if (g == 0 && start < sb.first_ino) start = sb.first_ino;
        if (!bread(bg.inode_bitmap, blk_buf)) continue;
        for (i = start; i <= end; i++) {
            u32 bit = (i - 1) % sb.inodes_per_group;
            if (!bitmap_test(blk_buf, bit)) {
                bitmap_set(blk_buf, bit);
                if (!bwrite(bg.inode_bitmap, blk_buf)) return 0;
                if (bg.free_inodes > 0) bg.free_inodes--;
                bg_store(g, &bg);
                if (sb.free_inodes > 0) sb.free_inodes--;
                sb_write_counts(0xFFFF);
                return i;
            }
        }
    }
    return 0;
}

static void free_inode_bit(u32 ino) {
    struct ext2_bg bg;
    u32 g;
    u32 bit;
    if (ino == 0 || ino > sb.inodes_count) return;
    g = group_of_inode(ino);
    if (!bg_load(g, &bg)) return;
    bit = (ino - 1) % sb.inodes_per_group;
    if (!bread(bg.inode_bitmap, blk_buf)) return;
    bitmap_clr(blk_buf, bit);
    bwrite(bg.inode_bitmap, blk_buf);
    bg.free_inodes++;
    bg_store(g, &bg);
    sb.free_inodes++;
    sb_write_counts(0xFFFF);
}

static void free_block_bit(u32 blk) {
    struct ext2_bg bg;
    u32 g;
    u32 local;
    u32 base;
    if (blk < sb.first_data_block || blk >= sb.blocks_count) return;
    if (blk <= sb.bg_block) return;          /* never free SB / BGDT */
    g = group_of_block(blk);
    if (!bg_load(g, &bg)) return;
    {
        u32 it_blocks = (sb.inodes_per_group * sb.inode_size + sb.block_size - 1)
                        / sb.block_size;
        if (blk == bg.block_bitmap || blk == bg.inode_bitmap) return;
        if (blk >= bg.inode_table && blk < bg.inode_table + it_blocks) return;
    }
    base = sb.first_data_block + g * sb.blocks_per_group;
    local = (sb.groups_count == 1) ? blk : (blk - base);
    if (!bread(bg.block_bitmap, blk_buf)) return;
    bitmap_clr(blk_buf, local);
    bwrite(bg.block_bitmap, blk_buf);
    bg.free_blocks++;
    bg_store(g, &bg);
    sb.free_blocks++;
    sb_write_counts(0xFFFF);
}

static u32 inode_block_of(u32 ino, u32 *offset_out) {
    struct ext2_bg bg;
    u32 g = group_of_inode(ino);
    u32 idx = (ino - 1) % sb.inodes_per_group;
    u32 ipb = sb.block_size / sb.inode_size;
    if (!bg_load(g, &bg)) return 0;
    *offset_out = (idx % ipb) * sb.inode_size;
    return bg.inode_table + idx / ipb;
}

static u32 inode_read(u32 ino, u8 *raw) {
    u32 off;
    u32 blk = inode_block_of(ino, &off);
    u32 i;
    if (!bread(blk, blk_buf)) return 0;
    for (i = 0; i < 128; i++) raw[i] = blk_buf[off + i];
    return 1;
}

static u32 inode_write(u32 ino, const u8 *raw) {
    u32 off;
    u32 blk = inode_block_of(ino, &off);
    u32 i;
    if (!bread(blk, blk_buf)) return 0;
    for (i = 0; i < 128; i++) blk_buf[off + i] = raw[i];
    return bwrite(blk, blk_buf);
}


/* Release every data and indirect block referenced by an inode (direct,
 * single-, double-, and triple-indirect). Leaves the raw inode image
 * zeroed for the caller to write back. */
static void inode_free_all_blocks(u8 *raw) {
    u32 b;
    u32 nent = sb.block_size / 4;
    for (b = 0; b < 12; b++) {
        u32 phys = rd32(raw + I_BLOCK + b * 4);
        if (phys) {
            free_block_bit(phys);
            wr32(raw + I_BLOCK + b * 4, 0);
        }
    }
    {
        u32 ind = rd32(raw + I_BLOCK + 12 * 4);
        if (ind) {
            u32 j;
            if (bread(ind, blk_buf)) {
                for (j = 0; j < nent; j++) {
                    u32 p = rd32(blk_buf + j * 4);
                    if (p) free_block_bit(p);
                }
            }
            free_block_bit(ind);
            wr32(raw + I_BLOCK + 12 * 4, 0);
        }
    }
    {
        u32 dind = rd32(raw + I_BLOCK + 13 * 4);
        if (dind) {
            u32 i;
            if (bread(dind, ind_buf)) {
                for (i = 0; i < nent; i++) {
                    u32 leaf = rd32(ind_buf + i * 4);
                    u32 j;
                    if (leaf == 0) continue;
                    if (bread(leaf, blk_buf)) {
                        for (j = 0; j < nent; j++) {
                            u32 p = rd32(blk_buf + j * 4);
                            if (p) free_block_bit(p);
                        }
                    }
                    free_block_bit(leaf);
                }
            }
            free_block_bit(dind);
            wr32(raw + I_BLOCK + 13 * 4, 0);
        }
    }
    {
        u32 tind = rd32(raw + I_BLOCK + 14 * 4);
        if (tind) {
            u32 a;
            if (bread(tind, tind_buf)) {
                for (a = 0; a < nent; a++) {
                    u32 dind = rd32(tind_buf + a * 4);
                    u32 i;
                    if (dind == 0) continue;
                    if (bread(dind, ind_buf)) {
                        for (i = 0; i < nent; i++) {
                            u32 leaf = rd32(ind_buf + i * 4);
                            u32 j;
                            if (leaf == 0) continue;
                            if (bread(leaf, blk_buf)) {
                                for (j = 0; j < nent; j++) {
                                    u32 p = rd32(blk_buf + j * 4);
                                    if (p) free_block_bit(p);
                                }
                            }
                            free_block_bit(leaf);
                        }
                    }
                    free_block_bit(dind);
                }
            }
            free_block_bit(tind);
            wr32(raw + I_BLOCK + 14 * 4, 0);
        }
    }
    wr32(raw + I_SIZE_LO, 0);
    wr32(raw + I_BLOCKS, 0);
}

static u32 inode_get_block(const u8 *raw, u32 file_block) {
    u32 ptrs = sb.block_size / 4;
    u32 single_max = 12 + ptrs;
    u32 double_max = single_max + ptrs * ptrs;
    u32 triple_max = double_max + ptrs * ptrs * ptrs;
    if (file_block < 12) {
        return rd32(raw + I_BLOCK + file_block * 4);
    }
    if (file_block < single_max) {
        u32 ind = rd32(raw + I_BLOCK + 12 * 4);
        if (ind == 0) return 0;
        if (!bread(ind, blk_buf)) return 0;
        return rd32(blk_buf + (file_block - 12) * 4);
    }
    if (file_block < double_max) {
        u32 dind = rd32(raw + I_BLOCK + 13 * 4);
        u32 rel;
        u32 lvl1;
        u32 lvl2;
        u32 leaf;
        if (dind == 0) return 0;
        rel = file_block - single_max;
        lvl1 = rel / ptrs;
        lvl2 = rel % ptrs;
        if (!bread(dind, ind_buf)) return 0;
        leaf = rd32(ind_buf + lvl1 * 4);
        if (leaf == 0) return 0;
        if (!bread(leaf, blk_buf)) return 0;
        return rd32(blk_buf + lvl2 * 4);
    }
    if (file_block < triple_max) {
        u32 tind = rd32(raw + I_BLOCK + 14 * 4);
        u32 rel;
        u32 lvl1;
        u32 rem;
        u32 lvl2;
        u32 lvl3;
        u32 dind;
        u32 leaf;
        if (tind == 0) return 0;
        rel = file_block - double_max;
        lvl1 = rel / (ptrs * ptrs);
        rem = rel % (ptrs * ptrs);
        lvl2 = rem / ptrs;
        lvl3 = rem % ptrs;
        if (!bread(tind, tind_buf)) return 0;
        dind = rd32(tind_buf + lvl1 * 4);
        if (dind == 0) return 0;
        if (!bread(dind, ind_buf)) return 0;
        leaf = rd32(ind_buf + lvl2 * 4);
        if (leaf == 0) return 0;
        if (!bread(leaf, blk_buf)) return 0;
        return rd32(blk_buf + lvl3 * 4);
    }
    return 0;
}

static void inode_add_iblocks(u8 *raw) {
    u32 ib = rd32(raw + I_BLOCKS);
    wr32(raw + I_BLOCKS, ib + sb.block_size / 512);
}

static u32 inode_set_block(u8 *raw, u32 file_block, u32 phys) {
    u32 ptrs = sb.block_size / 4;
    u32 single_max = 12 + ptrs;
    u32 double_max = single_max + ptrs * ptrs;
    u32 triple_max = double_max + ptrs * ptrs * ptrs;
    if (file_block < 12) {
        wr32(raw + I_BLOCK + file_block * 4, phys);
        return 1;
    }
    if (file_block < single_max) {
        u32 ind = rd32(raw + I_BLOCK + 12 * 4);
        u32 idx;
        if (ind == 0) {
            ind = alloc_block();
            if (ind == 0) return 0;
            wr32(raw + I_BLOCK + 12 * 4, ind);
            inode_add_iblocks(raw);
        }
        if (!bread(ind, blk_buf)) return 0;
        idx = file_block - 12;
        wr32(blk_buf + idx * 4, phys);
        return bwrite(ind, blk_buf);
    }
    if (file_block < double_max) {
        u32 dind = rd32(raw + I_BLOCK + 13 * 4);
        u32 rel;
        u32 lvl1;
        u32 lvl2;
        u32 leaf;
        if (dind == 0) {
            dind = alloc_block();
            if (dind == 0) return 0;
            wr32(raw + I_BLOCK + 13 * 4, dind);
            inode_add_iblocks(raw);
        }
        rel = file_block - single_max;
        lvl1 = rel / ptrs;
        lvl2 = rel % ptrs;
        if (!bread(dind, ind_buf)) return 0;
        leaf = rd32(ind_buf + lvl1 * 4);
        if (leaf == 0) {
            leaf = alloc_block();
            if (leaf == 0) return 0;
            wr32(ind_buf + lvl1 * 4, leaf);
            if (!bwrite(dind, ind_buf)) return 0;
            inode_add_iblocks(raw);
        }
        if (!bread(leaf, blk_buf)) return 0;
        wr32(blk_buf + lvl2 * 4, phys);
        return bwrite(leaf, blk_buf);
    }
    if (file_block < triple_max) {
        u32 tind = rd32(raw + I_BLOCK + 14 * 4);
        u32 rel;
        u32 lvl1;
        u32 rem;
        u32 lvl2;
        u32 lvl3;
        u32 dind;
        u32 leaf;
        if (tind == 0) {
            tind = alloc_block();
            if (tind == 0) return 0;
            wr32(raw + I_BLOCK + 14 * 4, tind);
            inode_add_iblocks(raw);
        }
        rel = file_block - double_max;
        lvl1 = rel / (ptrs * ptrs);
        rem = rel % (ptrs * ptrs);
        lvl2 = rem / ptrs;
        lvl3 = rem % ptrs;
        if (!bread(tind, tind_buf)) return 0;
        dind = rd32(tind_buf + lvl1 * 4);
        if (dind == 0) {
            dind = alloc_block();
            if (dind == 0) return 0;
            wr32(tind_buf + lvl1 * 4, dind);
            if (!bwrite(tind, tind_buf)) return 0;
            inode_add_iblocks(raw);
        }
        if (!bread(dind, ind_buf)) return 0;
        leaf = rd32(ind_buf + lvl2 * 4);
        if (leaf == 0) {
            leaf = alloc_block();
            if (leaf == 0) return 0;
            wr32(ind_buf + lvl2 * 4, leaf);
            if (!bwrite(dind, ind_buf)) return 0;
            inode_add_iblocks(raw);
        }
        if (!bread(leaf, blk_buf)) return 0;
        wr32(blk_buf + lvl3 * 4, phys);
        return bwrite(leaf, blk_buf);
    }
    return 0;
}

static u32 inode_map(u8 *raw, u32 file_block, u32 do_alloc) {
    u32 b = inode_get_block(raw, file_block);
    u32 ib;
    if (b != 0 || !do_alloc) return b;
    b = alloc_block();
    if (b == 0) return 0;
    if (!inode_set_block(raw, file_block, b)) return 0;
    ib = rd32(raw + I_BLOCKS);
    wr32(raw + I_BLOCKS, ib + sb.block_size / 512);
    return b;
}

static u32 dir_find(u32 dir_ino, const char *name, u32 nlen, u32 *out_ino) {
    u32 size;
    u32 fblk;
    u32 mode;
    if (!inode_read(dir_ino, ino_raw)) return 0;
    mode = rd16(ino_raw + I_MODE);
    if ((mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
    size = rd32(ino_raw + I_SIZE_LO);
    for (fblk = 0; fblk * sb.block_size < size; fblk++) {
        u32 phys = inode_get_block(ino_raw, fblk);
        u32 pos = 0;
        if (phys == 0) continue;
        if (!bread(phys, blk_buf)) return 0;
        while (pos + 8 <= sb.block_size) {
            u32 ino = rd32(blk_buf + pos);
            u16 reclen = rd16(blk_buf + pos + 4);
            u8 namelen = blk_buf[pos + 6];
            if (reclen < 8 || pos + reclen > sb.block_size) break;
            if (ino != 0 && name_eq((const char *)(blk_buf + pos + 8),
                                    namelen, name, nlen)) {
                *out_ino = ino;
                return 1;
            }
            pos += reclen;
        }
    }
    return 0;
}

static u32 dir_add(u32 dir_ino, const char *name, u32 nlen, u32 child, u8 ftype) {
    u32 size;
    u32 fblk;
    u32 need;
    u32 mode;
    if (nlen == 0 || nlen > NAME_MAX) return 0;
    if (!inode_read(dir_ino, ino_raw)) return 0;
    mode = rd16(ino_raw + I_MODE);
    if ((mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
    size = rd32(ino_raw + I_SIZE_LO);
    need = (8 + nlen + 3) & ~3u;

    fblk = 0;
    while (1) {
        u32 phys;
        u32 pos;
        u32 new_block = 0;
        if (fblk * sb.block_size >= size) {
            new_block = 1;
        }
        phys = inode_map(ino_raw, fblk, 1);
        if (phys == 0) return 0;
        if (new_block) {
            u32 z;
            for (z = 0; z < sb.block_size; z++) blk_buf[z] = 0;
            wr32(blk_buf + 0, 0);
            wr16(blk_buf + 4, (u16)sb.block_size);
            size = (fblk + 1) * sb.block_size;
            wr32(ino_raw + I_SIZE_LO, size);
        } else {
            if (!bread(phys, blk_buf)) return 0;
        }
        pos = 0;
        while (pos + 8 <= sb.block_size) {
            u32 ino = rd32(blk_buf + pos);
            u16 reclen = rd16(blk_buf + pos + 4);
            u8 namelen = blk_buf[pos + 6];
            u16 real;
            u16 remain;
            if (reclen < 8 || pos + reclen > sb.block_size) break;
            real = (u16)((8 + namelen + 3) & ~3u);
            if (ino == 0 && reclen >= need) {
                u32 k;
                wr32(blk_buf + pos, child);
                blk_buf[pos + 6] = (u8)nlen;
                blk_buf[pos + 7] = ftype;
                for (k = 0; k < nlen; k++) {
                    blk_buf[pos + 8 + k] = (u8)name[k];
                }
                if (!bwrite(phys, blk_buf)) return 0;
                return inode_write(dir_ino, ino_raw);
            }
            remain = (u16)(reclen - real);
            if (ino != 0 && remain >= need) {
                u32 np = pos + real;
                u32 k;
                wr16(blk_buf + pos + 4, real);
                wr32(blk_buf + np, child);
                wr16(blk_buf + np + 4, remain);
                blk_buf[np + 6] = (u8)nlen;
                blk_buf[np + 7] = ftype;
                for (k = 0; k < nlen; k++) {
                    blk_buf[np + 8 + k] = (u8)name[k];
                }
                if (!bwrite(phys, blk_buf)) return 0;
                return inode_write(dir_ino, ino_raw);
            }
            pos += reclen;
        }
        if (!new_block) {
            /* This block was full; extend size so next iteration allocates. */
            size = (fblk + 1) * sb.block_size;
            wr32(ino_raw + I_SIZE_LO, size);
            if (!inode_write(dir_ino, ino_raw)) return 0;
        } else {
            /* Fresh block should always have had space. */
            return 0;
        }
        fblk++;
    }
}

static u32 dir_remove(u32 dir_ino, const char *name, u32 nlen, u32 *out_ino) {
    u32 size;
    u32 fblk;
    if (!inode_read(dir_ino, ino_raw)) return 0;
    size = rd32(ino_raw + I_SIZE_LO);
    for (fblk = 0; fblk * sb.block_size < size; fblk++) {
        u32 phys = inode_get_block(ino_raw, fblk);
        u32 pos = 0;
        u32 prev = 0xFFFFFFFF;
        if (phys == 0) continue;
        if (!bread(phys, blk_buf)) return 0;
        while (pos + 8 <= sb.block_size) {
            u32 ino = rd32(blk_buf + pos);
            u16 reclen = rd16(blk_buf + pos + 4);
            u8 namelen = blk_buf[pos + 6];
            if (reclen < 8 || pos + reclen > sb.block_size) break;
            if (ino != 0 && name_eq((const char *)(blk_buf + pos + 8),
                                    namelen, name, nlen)) {
                *out_ino = ino;
                wr32(blk_buf + pos, 0);
                if (prev != 0xFFFFFFFF) {
                    u16 prev_len = rd16(blk_buf + prev + 4);
                    wr16(blk_buf + prev + 4, (u16)(prev_len + reclen));
                }
                return bwrite(phys, blk_buf);
            }
            if (ino != 0) prev = pos;
            pos += reclen;
        }
    }
    return 0;
}

/* Walk a path relative to the Ext2 root. Leading slashes ignored. */
static u32 walk_rel(const char *rel, u32 *is_dir) {
    u32 cur = EXT2_ROOT_INO;
    u32 i = 0;
    if (rel == 0) return 0;
    while (rel[i] != 0) {
        u32 n;
        u32 nxt;
        while (rel[i] == '/') i++;
        if (rel[i] == 0) break;
        n = 0;
        while (rel[i + n] != 0 && rel[i + n] != '/') n++;
        if (n == 1 && rel[i] == '.') {
            /* stay */
        } else if (n == 2 && rel[i] == '.' && rel[i + 1] == '.') {
            if (cur != EXT2_ROOT_INO) {
                if (!dir_find(cur, "..", 2, &nxt)) return 0;
                cur = nxt;
            }
        } else {
            if (!dir_find(cur, &rel[i], n, &nxt)) return 0;
            cur = nxt;
        }
        i += n;
    }
    if (!inode_read(cur, ino_raw)) return 0;
    if (is_dir) {
        *is_dir = ((rd16(ino_raw + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
    }
    return cur;
}

/* Split "/mnt/foo/bar" -> parent ino of "bar" + name. Creates nothing. */
static u32 walk_parent(const char *rel, char *last, u32 *last_n) {
    u32 cur = EXT2_ROOT_INO;
    u32 i = 0;
    u32 start = 0;
    u32 n = 0;
    u32 saw = 0;
    if (rel == 0) return 0;
    while (rel[i] != 0) {
        while (rel[i] == '/') i++;
        if (rel[i] == 0) break;
        start = i;
        n = 0;
        while (rel[i] != 0 && rel[i] != '/') {
            i++;
            n++;
        }
        saw = 1;
        if (rel[i] == 0) {
            /* final component */
            u32 k;
            *last_n = n;
            for (k = 0; k < n; k++) last[k] = rel[start + k];
            last[n] = 0;
            return cur;
        }
        /* intermediate */
        if (!(n == 1 && rel[start] == '.')) {
            u32 nxt;
            if (n == 2 && rel[start] == '.' && rel[start + 1] == '.') {
                if (cur != EXT2_ROOT_INO) {
                    if (!dir_find(cur, "..", 2, &nxt)) return 0;
                    cur = nxt;
                }
            } else {
                if (!dir_find(cur, &rel[start], n, &nxt)) return 0;
                cur = nxt;
            }
        }
    }
    (void)saw;
    return 0;
}

/* Strip the "/mnt" prefix; returns pointer into path, or 0 if not under /mnt. */
static const char *mnt_rel(const char *path) {
    if (path == 0 || path[0] != '/') return 0;
    /* accept "/mnt" or "/mnt/..." */
    if (path[1] != 'm' || path[2] != 'n' || path[3] != 't') return 0;
    if (path[4] == 0) return path + 4;          /* empty rel -> root */
    if (path[4] != '/') return 0;
    return path + 5;
}

/* Daily-driver UX: /home and /home/... alias the Ext2 volume path "home". */
static const char *home_rel(const char *path) {
    if (path == 0 || path[0] != '/') return 0;
    if (path[1] != 'h' || path[2] != 'o' || path[3] != 'm' || path[4] != 'e') {
        return 0;
    }
    if (path[5] == 0) {
        return path + 1;                 /* "home" */
    }
    if (path[5] != '/') return 0;
    return path + 1;                     /* "home/..." */
}

/* Ramfs keeps the live root listing and these overlays; everything else
 * (when Ext2 is mounted) lands on the Ext2 volume — a step toward true root. */
static u32 ramfs_overlay(const char *path) {
    /* "/", "/bin", "/etc", "/tmp", "/proc", "/src", "/ref", "/asm", "/dev" */
    if (path == 0 || path[0] != '/') return 1;
    if (path[1] == 0) return 1;          /* "/" stays Ramfs for ls / */
    if (path[1] == 'b' && path[2] == 'i' && path[3] == 'n' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 'e' && path[2] == 't' && path[3] == 'c' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 'p' && path[2] == 'r' && path[3] == 'o' && path[4] == 'c' &&
        (path[5] == 0 || path[5] == '/')) return 1;
    if (path[1] == 's' && path[2] == 'r' && path[3] == 'c' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 'r' && path[2] == 'e' && path[3] == 'f' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 'a' && path[2] == 's' && path[3] == 'm' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    if (path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
        (path[4] == 0 || path[4] == '/')) return 1;
    return 0;
}

/* Volume-relative path for any Ext2-owned absolute path, or 0. */
static const char *persist_rel(const char *path) {
    const char *r = mnt_rel(path);
    if (r) return r;
    if (!sb.mounted) return 0;
    r = home_rel(path);
    if (r) return r;
    if (ramfs_overlay(path)) return 0;
    /* Strip leading '/': "/notes" -> "notes", path always absolute here. */
    return path + 1;
}

u32 ext2_is_mnt_path(const char *path) {
    return persist_rel(path) != 0;
}

u32 ext2_mounted(void) {
    return sb.mounted;
}

u32 ext2_fh_is(u32 fh) {
    return (fh & FH_BASE) != 0;
}

static u32 fh_slot(u32 fh) {
    return fh & ~FH_BASE;
}

static u32 alloc_fh(void) {
    u32 i;
    for (i = 1; i < MAX_OPEN; i++) {
        if (!opens[i].used) {
            opens[i].used = 1;
            opens[i].refs = 1;
            return FH_BASE | i;
        }
    }
    return 0;
}

u32 ext2_mount(void) {
    u8 sbsec[1024];
    u32 i;
    u32 bg0;

    sb.mounted = 0;
    for (i = 0; i < MAX_OPEN; i++) opens[i].used = 0;

    if (!drivers.blk_present()) {
        return 0;
    }

    /* Superblock lives at byte offset 1024 (sectors 2..3 for 512B). */
    if (!drivers.blk_read(2, sbsec, 2)) {
        kprintf("[EXT2] failed to read superblock\n");
        return 0;
    }

    sb.magic = rd16(sbsec + 56);
    if (sb.magic != EXT2_MAGIC) {
        kprintf("[EXT2] bad magic 0x%x\n", (u64)sb.magic);
        return 0;
    }
    sb.inodes_count = rd32(sbsec + 0);
    sb.blocks_count = rd32(sbsec + 4);
    sb.first_data_block = rd32(sbsec + 20);
    sb.log_block_size = rd32(sbsec + 24);
    sb.blocks_per_group = rd32(sbsec + 32);
    sb.inodes_per_group = rd32(sbsec + 40);
    sb.inode_size = rd16(sbsec + 88);
    if (sb.inode_size == 0) sb.inode_size = 128;
    sb.first_ino = rd32(sbsec + 84);
    if (sb.first_ino == 0) sb.first_ino = 11;
    sb.block_size = 1024u << sb.log_block_size;
    if (sb.block_size != 1024 && sb.block_size != 2048 && sb.block_size != 4096) {
        kprintf("[EXT2] unsupported block size %u\n", (u64)sb.block_size);
        return 0;
    }

    sb.bg_block = sb.first_data_block + 1;
    if (sb.blocks_per_group == 0) {
        kprintf("[EXT2] blocks_per_group is 0\n");
        return 0;
    }
    sb.groups_count = (sb.blocks_count - sb.first_data_block + sb.blocks_per_group - 1)
                      / sb.blocks_per_group;
    if (sb.groups_count == 0) sb.groups_count = 1;
    sb.free_blocks = rd32(sbsec + 12);
    sb.free_inodes = rd32(sbsec + 16);

    if (!bread(sb.bg_block, blk_buf)) {
        kprintf("[EXT2] failed to read block group descriptor\n");
        return 0;
    }
    /* Probe group 0 descriptor presence (bitmap/itable non-zero). */
    if (rd32(blk_buf + 0) == 0 || rd32(blk_buf + 8) == 0) {
        kprintf("[EXT2] bad group-0 descriptor\n");
        return 0;
    }

    /* Verify root inode is a directory. */
    if (!inode_read(EXT2_ROOT_INO, ino_raw)) {
        kprintf("[EXT2] cannot read root inode\n");
        return 0;
    }
    if ((rd16(ino_raw + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        kprintf("[EXT2] root is not a directory\n");
        return 0;
    }

    sb.mounted = 1;
    /* Hardening: mark filesystem not-valid until sync (s_state=2). */
    sb_write_counts(2);
    bg0 = sb.bg_block;
    kprintf("[EXT2] mounted: blocks=%u inodes=%u blksize=%u groups=%u bg0=%u /mnt+/home ready\n",
            (u64)sb.blocks_count, (u64)sb.inodes_count,
            (u64)sb.block_size, (u64)sb.groups_count, (u64)bg0);
    return 1;
}

u32 ext2_open(const char *path, u32 flags) {
    const char *rel;
    char last[NAME_MAX + 1];
    u32 last_n;
    u32 parent;
    u32 ino;
    u32 is_dir;
    u32 fh;
    u32 acc = flags & 3;
    u32 want_creat = (flags & O_CREAT) != 0;
    u32 want_trunc = (flags & O_TRUNC) != 0;

    if (!sb.mounted) return 0;
    rel = persist_rel(path);
    if (rel == 0) return 0;

    /* Open the mount root itself. */
    if (rel[0] == 0) {
        if (acc == O_WRONLY) return 0;
        if (!inode_read(EXT2_ROOT_INO, ino_raw)) return 0;
        fh = alloc_fh();
        if (fh == 0) return 0;
        opens[fh_slot(fh)].ino = EXT2_ROOT_INO;
        opens[fh_slot(fh)].mode = rd16(ino_raw + I_MODE);
        opens[fh_slot(fh)].readable = 1;
        opens[fh_slot(fh)].writable = 0;
        opens[fh_slot(fh)].append = 0;
        opens[fh_slot(fh)].off = 0;
        opens[fh_slot(fh)].size = rd32(ino_raw + I_SIZE_LO);
        return fh;
    }

    ino = walk_rel(rel, &is_dir);
    if (ino == 0) {
        if (!want_creat) return 0;
        parent = walk_parent(rel, last, &last_n);
        if (parent == 0 || last_n == 0) return 0;
        /* Create a regular file. */
        {
            u32 neu = alloc_inode_num();
            u32 k;
            if (neu == 0) return 0;
            for (k = 0; k < 128; k++) ino_raw[k] = 0;
            wr16(ino_raw + I_MODE, (u16)(EXT2_S_IFREG | 0x1A4));
            wr16(ino_raw + I_LINKS, 1);
            wr32(ino_raw + I_SIZE_LO, 0);
            wr32(ino_raw + I_BLOCKS, 0);
            if (!inode_write(neu, ino_raw)) return 0;
            if (!dir_add(parent, last, last_n, neu, EXT2_FT_REG)) return 0;
            ino = neu;
            is_dir = 0;
        }
    }

    if (is_dir && acc != O_RDONLY) return 0;
    if (!inode_read(ino, ino_raw)) return 0;

    if (want_trunc && !is_dir && (acc == O_WRONLY || acc == O_RDWR)) {
        inode_free_all_blocks(ino_raw);
        if (!inode_write(ino, ino_raw)) return 0;
    }

    fh = alloc_fh();
    if (fh == 0) return 0;
    opens[fh_slot(fh)].ino = ino;
    opens[fh_slot(fh)].mode = rd16(ino_raw + I_MODE);
    opens[fh_slot(fh)].readable = (acc == O_RDONLY || acc == O_RDWR) ? 1 : 0;
    opens[fh_slot(fh)].writable = (acc == O_WRONLY || acc == O_RDWR) ? 1 : 0;
    opens[fh_slot(fh)].append = (flags & O_APPEND) ? 1 : 0;
    opens[fh_slot(fh)].off = 0;
    opens[fh_slot(fh)].size = rd32(ino_raw + I_SIZE_LO);
    return fh;
}

void ext2_file_retain(u32 fh) {
    u32 s;
    if (!ext2_fh_is(fh)) return;
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used) return;
    opens[s].refs++;
}

u32 ext2_close(u32 fh) {
    u32 s;
    if (!ext2_fh_is(fh)) return 0;
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used) return 0;
    if (opens[s].refs > 1) {
        opens[s].refs--;
        return 1;
    }
    opens[s].refs = 0;
    opens[s].used = 0;
    return 1;
}

u32 ext2_file_is_dir(u32 fh) {
    u32 s;
    if (!ext2_fh_is(fh)) return 0;
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used) return 0;
    return ((opens[s].mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
}

u64 ext2_lseek(u32 fh, u64 off, u32 whence) {
    u32 s;
    u64 neu;
    if (!ext2_fh_is(fh)) return (u64)(-1);
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used) return (u64)(-1);
    if (whence == SEEK_SET) neu = off;
    else if (whence == SEEK_CUR) neu = opens[s].off + off;
    else if (whence == SEEK_END) neu = opens[s].size + off;
    else return (u64)(-1);
    opens[s].off = neu;
    return neu;
}

u64 ext2_read(u32 fh, char *kbuf, u64 len) {
    u32 s;
    u64 done = 0;
    u8 raw[128];
    if (!ext2_fh_is(fh)) return (u64)(-1);
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used || !opens[s].readable) return (u64)(-1);
    if ((opens[s].mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return (u64)(-1);
    if (!inode_read(opens[s].ino, raw)) return (u64)(-1);
    opens[s].size = rd32(raw + I_SIZE_LO);
    while (done < len && opens[s].off < opens[s].size) {
        u32 fblk = (u32)(opens[s].off / sb.block_size);
        u32 boff = (u32)(opens[s].off % sb.block_size);
        u32 phys = inode_get_block(raw, fblk);
        u64 can = sb.block_size - boff;
        u64 remain = opens[s].size - opens[s].off;
        u64 take;
        u64 k;
        if (can > remain) can = remain;
        take = len - done;
        if (take > can) take = can;
        if (phys == 0) {
            for (k = 0; k < take; k++) kbuf[done + k] = 0;
        } else {
            if (!bread(phys, blk_buf)) return done != 0 ? done : (u64)(-1);
            for (k = 0; k < take; k++) kbuf[done + k] = (char)blk_buf[boff + k];
        }
        opens[s].off += take;
        done += take;
    }
    return done;
}

u64 ext2_write(u32 fh, const char *kbuf, u64 len) {
    u32 s;
    u64 done = 0;
    u8 raw[128];
    if (!ext2_fh_is(fh)) return (u64)(-1);
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used || !opens[s].writable) return (u64)(-1);
    if ((opens[s].mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return (u64)(-1);
    if (!inode_read(opens[s].ino, raw)) return (u64)(-1);
    opens[s].size = rd32(raw + I_SIZE_LO);
    if (opens[s].append) opens[s].off = opens[s].size;

    while (done < len) {
        u32 fblk = (u32)(opens[s].off / sb.block_size);
        u32 boff = (u32)(opens[s].off % sb.block_size);
        u32 phys = inode_map(raw, fblk, 1);
        u64 can = sb.block_size - boff;
        u64 take = len - done;
        u64 k;
        if (phys == 0) return done != 0 ? done : (u64)(-1);
        if (take > can) take = can;
        if (boff != 0 || take != sb.block_size) {
            if (!bread(phys, blk_buf)) return done != 0 ? done : (u64)(-1);
        }
        for (k = 0; k < take; k++) blk_buf[boff + k] = (u8)kbuf[done + k];
        if (!bwrite(phys, blk_buf)) return done != 0 ? done : (u64)(-1);
        opens[s].off += take;
        done += take;
        if (opens[s].off > opens[s].size) {
            opens[s].size = opens[s].off;
            wr32(raw + I_SIZE_LO, (u32)opens[s].size);
        }
    }
    if (!inode_write(opens[s].ino, raw)) {
        return done != 0 ? done : (u64)(-1);
    }
    return done;
}

u64 ext2_getdents(u32 fh, char *kbuf, u64 len) {
    u32 s;
    u8 raw[128];
    u32 size;
    u32 fblk;
    u64 produced = 0;
    u64 skip;
    if (!ext2_fh_is(fh)) return (u64)(-1);
    s = fh_slot(fh);
    if (s >= MAX_OPEN || !opens[s].used) return (u64)(-1);
    if ((opens[s].mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return (u64)(-1);
    if (!inode_read(opens[s].ino, raw)) return (u64)(-1);
    size = rd32(raw + I_SIZE_LO);
    skip = opens[s].off;

    for (fblk = 0; fblk * sb.block_size < size; fblk++) {
        u32 phys = inode_get_block(raw, fblk);
        u32 pos = 0;
        if (phys == 0) continue;
        if (!bread(phys, blk_buf)) return (u64)(-1);
        while (pos + 8 <= sb.block_size) {
            u32 ino = rd32(blk_buf + pos);
            u16 reclen = rd16(blk_buf + pos + 4);
            u8 namelen = blk_buf[pos + 6];
            u8 ftype = blk_buf[pos + 7];
            if (reclen < 8 || pos + reclen > sb.block_size) break;
            if (ino != 0) {
                if (skip > 0) {
                    skip--;
                } else {
                    u32 k;
                    if (produced + DENT_RECLEN > len) {
                        opens[s].off += produced / DENT_RECLEN;
                        return produced;
                    }
                    /* Record: ino:u32, reclen:u16=48, type:u8, namelen:u8, name */
                    wr32((u8 *)(kbuf + produced), ino);
                    wr16((u8 *)(kbuf + produced + 4), (u16)DENT_RECLEN);
                    kbuf[produced + 6] = (char)((ftype == EXT2_FT_DIR) ? 4 : 8);
                    kbuf[produced + 7] = (char)namelen;
                    for (k = 0; k < 40; k++) kbuf[produced + 8 + k] = 0;
                    for (k = 0; k < namelen && k < 39; k++) {
                        kbuf[produced + 8 + k] = (char)blk_buf[pos + 8 + k];
                    }
                    produced += DENT_RECLEN;
                }
            }
            pos += reclen;
        }
    }
    opens[s].off += produced / DENT_RECLEN;
    return produced;
}

u32 ext2_mkdir(const char *path) {
    const char *rel;
    char last[NAME_MAX + 1];
    u32 last_n;
    u32 parent;
    u32 neu;
    u32 k;
    u32 phys;
    if (!sb.mounted) return 0;
    rel = persist_rel(path);
    if (rel == 0 || rel[0] == 0) return 0;
    if (walk_rel(rel, 0) != 0) return 0;       /* exists */
    parent = walk_parent(rel, last, &last_n);
    if (parent == 0 || last_n == 0) return 0;
    neu = alloc_inode_num();
    if (neu == 0) return 0;
    for (k = 0; k < 128; k++) ino_raw[k] = 0;
    wr16(ino_raw + I_MODE, (u16)(EXT2_S_IFDIR | 0x1ED));
    wr16(ino_raw + I_LINKS, 2);                /* . and parent link */
    wr32(ino_raw + I_SIZE_LO, sb.block_size);
    phys = alloc_block();
    if (phys == 0) return 0;
    wr32(ino_raw + I_BLOCK, phys);
    wr32(ino_raw + I_BLOCKS, sb.block_size / 512);
    /* Build . and .. entries. */
    for (k = 0; k < sb.block_size; k++) blk_buf[k] = 0;
    wr32(blk_buf + 0, neu);
    wr16(blk_buf + 4, 12);
    blk_buf[6] = 1;
    blk_buf[7] = EXT2_FT_DIR;
    blk_buf[8] = '.';
    wr32(blk_buf + 12, parent);
    wr16(blk_buf + 16, (u16)(sb.block_size - 12));
    blk_buf[18] = 2;
    blk_buf[19] = EXT2_FT_DIR;
    blk_buf[20] = '.';
    blk_buf[21] = '.';
    if (!bwrite(phys, blk_buf)) return 0;
    if (!inode_write(neu, ino_raw)) return 0;
    if (!dir_add(parent, last, last_n, neu, EXT2_FT_DIR)) return 0;
    /* Bump parent link count. */
    if (!inode_read(parent, ino_raw)) return 0;
    wr16(ino_raw + I_LINKS, (u16)(rd16(ino_raw + I_LINKS) + 1));
    if (!inode_write(parent, ino_raw)) return 0;
    return 1;
}

static u32 dir_is_empty(u32 dir_ino) {
    u32 size;
    u32 fblk;
    if (!inode_read(dir_ino, ino_raw)) return 0;
    size = rd32(ino_raw + I_SIZE_LO);
    for (fblk = 0; fblk * sb.block_size < size; fblk++) {
        u32 phys = inode_get_block(ino_raw, fblk);
        u32 pos = 0;
        if (phys == 0) continue;
        if (!bread(phys, blk_buf)) return 0;
        while (pos + 8 <= sb.block_size) {
            u32 ino = rd32(blk_buf + pos);
            u16 reclen = rd16(blk_buf + pos + 4);
            u8 namelen = blk_buf[pos + 6];
            if (reclen < 8 || pos + reclen > sb.block_size) break;
            if (ino != 0) {
                /* Allow only "." and ".." */
                if (!(namelen == 1 && blk_buf[pos + 8] == '.')
                    && !(namelen == 2 && blk_buf[pos + 8] == '.'
                         && blk_buf[pos + 9] == '.')) {
                    return 0;
                }
            }
            pos += reclen;
        }
    }
    return 1;
}

u32 ext2_unlink(const char *path) {
    const char *rel;
    char last[NAME_MAX + 1];
    u32 last_n;
    u32 parent;
    u32 victim;
    u32 mode;
    u32 b;
    if (!sb.mounted) return 0;
    rel = persist_rel(path);
    if (rel == 0 || rel[0] == 0) return 0;
    parent = walk_parent(rel, last, &last_n);
    if (parent == 0 || last_n == 0) return 0;
    if (!dir_find(parent, last, last_n, &victim)) return 0;
    if (!inode_read(victim, ino_raw)) return 0;
    mode = rd16(ino_raw + I_MODE);
    if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        return 0;                      /* directories need rmdir */
    }
    if (!dir_remove(parent, last, last_n, &victim)) return 0;
    if (!inode_read(victim, ino_raw)) return 0;
    inode_free_all_blocks(ino_raw);
    for (b = 0; b < 128; b++) ino_raw[b] = 0;
    inode_write(victim, ino_raw);
    free_inode_bit(victim);
    return 1;
}

u32 ext2_rmdir(const char *path) {
    const char *rel;
    char last[NAME_MAX + 1];
    u32 last_n;
    u32 parent;
    u32 victim;
    u32 mode;
    u32 phys;
    if (!sb.mounted) return 0;
    rel = persist_rel(path);
    if (rel == 0 || rel[0] == 0) return 0;
    parent = walk_parent(rel, last, &last_n);
    if (parent == 0 || last_n == 0) return 0;
    if (!dir_find(parent, last, last_n, &victim)) return 0;
    if (!inode_read(victim, ino_raw)) return 0;
    mode = rd16(ino_raw + I_MODE);
    if ((mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
    if (!dir_is_empty(victim)) return 0;
    if (!dir_remove(parent, last, last_n, &victim)) return 0;
    if (!inode_read(victim, ino_raw)) return 0;
    phys = rd32(ino_raw + I_BLOCK);
    if (phys) free_block_bit(phys);
    /* Drop parent ".." link. */
    if (inode_read(parent, ino_raw)) {
        u16 links = rd16(ino_raw + I_LINKS);
        if (links > 1) {
            wr16(ino_raw + I_LINKS, (u16)(links - 1));
            inode_write(parent, ino_raw);
        }
    }
    {
        u32 z;
        for (z = 0; z < 128; z++) ino_raw[z] = 0;
    }
    inode_write(victim, ino_raw);
    free_inode_bit(victim);
    return 1;
}

u32 ext2_sync(void) {
    if (!sb.mounted) return 0;
    /* Write barrier: persist free counts + mark VALID_FS before FLUSH. */
    sb_write_counts(1);
    return drivers.blk_flush();
}

/* Kernel self-test: write /mnt/m6pass.txt, read it back, flush. */
void ext2_selftest(void) {
    u32 fh;
    u64 n;
    char buf[64];
    const char *msg = "m6-ext2-persistent-ok\n";
    u32 i;
    u32 len = 0;
    while (msg[len] != 0) len++;

    if (!sb.mounted) {
        kprintf("[EXT2] selftest skipped (not mounted)\n");
        return;
    }
    fh = ext2_open("/mnt/m6pass.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fh == 0) {
        kprintf("[EXT2] [FAIL] create /mnt/m6pass.txt\n");
        return;
    }
    n = ext2_write(fh, msg, len);
    if (n != len) {
        kprintf("[EXT2] [FAIL] write (%u)\n", n);
        ext2_close(fh);
        return;
    }
    ext2_lseek(fh, 0, SEEK_SET);
    for (i = 0; i < 64; i++) buf[i] = 0;
    n = ext2_read(fh, buf, 63);
    ext2_close(fh);
    ext2_sync();
    if (n != len) {
        kprintf("[EXT2] [FAIL] readback len %u\n", n);
        return;
    }
    for (i = 0; i < len; i++) {
        if (buf[i] != msg[i]) {
            kprintf("[EXT2] [FAIL] readback mismatch at %u\n", (u64)i);
            return;
        }
    }
    kprintf("[EXT2] [PASS] create/write/read/sync /mnt/m6pass.txt\n");

    /* Past single-indirect (268 KiB at 1 KiB blocks). Write full blocks to
     * avoid RMW amplification that wedges VirtIO-blk under TCG. */
    {
        u32 fh2;
        u32 total = 270 * 1024;
        u32 done = 0;
        u8 pattern[1024];
        u32 i;
        for (i = 0; i < 1024; i++) pattern[i] = (u8)(i & 0xFF);
        fh2 = ext2_open("/mnt/big.bin", O_CREAT | O_RDWR | O_TRUNC);
        if (fh2 == 0) {
            kprintf("[EXT2] [FAIL] create /mnt/big.bin\n");
            return;
        }
        while (done < total) {
            u32 chunk = 1024;
            u64 n;
            if (chunk > total - done) chunk = total - done;
            n = ext2_write(fh2, (char *)pattern, chunk);
            if (n != chunk) {
                kprintf("[EXT2] [FAIL] big write at %u got %u\n", (u64)done, n);
                ext2_close(fh2);
                return;
            }
            done += chunk;
        }
        if (ext2_lseek(fh2, 0, SEEK_SET) != 0) {
            kprintf("[EXT2] [FAIL] big lseek 0\n");
            ext2_close(fh2);
            return;
        }
        {
            u8 got[1024];
            u64 n = ext2_read(fh2, (char *)got, 1024);
            if (n != 1024) {
                kprintf("[EXT2] [FAIL] big head read %u\n", n);
                ext2_close(fh2);
                return;
            }
            for (i = 0; i < 1024; i++) {
                if (got[i] != pattern[i]) {
                    kprintf("[EXT2] [FAIL] big head mismatch\n");
                    ext2_close(fh2);
                    return;
                }
            }
            if (ext2_lseek(fh2, total - 1024, SEEK_SET) != (u64)(total - 1024)) {
                kprintf("[EXT2] [FAIL] big lseek tail\n");
                ext2_close(fh2);
                return;
            }
            n = ext2_read(fh2, (char *)got, 1024);
            if (n != 1024) {
                kprintf("[EXT2] [FAIL] big tail read %u\n", n);
                ext2_close(fh2);
                return;
            }
            for (i = 0; i < 1024; i++) {
                if (got[i] != pattern[i]) {
                    kprintf("[EXT2] [FAIL] big tail mismatch\n");
                    ext2_close(fh2);
                    return;
                }
            }
        }
        ext2_close(fh2);
        drivers.blk_flush();
        kprintf("[EXT2] [PASS] double-indirect /mnt/big.bin 270KiB\n");
    }

    /* Sparse write past double-indirect ceiling (1K: file block 65804)
     * exercises i_block[14] without needing a 64MiB image. */
    {
        u32 fh3;
        u32 ptrs = sb.block_size / 4;
        u32 single_max = 12 + ptrs;
        u32 double_max = single_max + ptrs * ptrs;
        u64 trip_off = (u64)double_max * (u64)sb.block_size;
        u8 pattern[1024];
        u8 got[1024];
        u32 i;
        u64 n;
        for (i = 0; i < 1024; i++) pattern[i] = (u8)(0xA5 ^ (i & 0xFF));
        fh3 = ext2_open("/mnt/trip.bin", O_CREAT | O_RDWR | O_TRUNC);
        if (fh3 == 0) {
            kprintf("[EXT2] [FAIL] create /mnt/trip.bin\n");
            return;
        }
        if (ext2_lseek(fh3, trip_off, SEEK_SET) != trip_off) {
            kprintf("[EXT2] [FAIL] trip lseek %u\n", (u64)trip_off);
            ext2_close(fh3);
            return;
        }
        {
            u32 chunk = sb.block_size;
            if (chunk > 1024) chunk = 1024;
            n = ext2_write(fh3, (char *)pattern, chunk);
            if (n != chunk) {
                kprintf("[EXT2] [FAIL] trip write got %u\n", n);
                ext2_close(fh3);
                return;
            }
            if (ext2_lseek(fh3, trip_off, SEEK_SET) != trip_off) {
                kprintf("[EXT2] [FAIL] trip re-lseek\n");
                ext2_close(fh3);
                return;
            }
            for (i = 0; i < 1024; i++) got[i] = 0;
            n = ext2_read(fh3, (char *)got, chunk);
            if (n != chunk) {
                kprintf("[EXT2] [FAIL] trip read %u\n", n);
                ext2_close(fh3);
                return;
            }
            for (i = 0; i < chunk; i++) {
                if (got[i] != pattern[i]) {
                    kprintf("[EXT2] [FAIL] trip mismatch at %u\n", (u64)i);
                    ext2_close(fh3);
                    return;
                }
            }
        }
        ext2_close(fh3);
        /* Unlink recycles the triple-indirect tree. */
        if (!ext2_unlink("/mnt/trip.bin")) {
            kprintf("[EXT2] [FAIL] unlink /mnt/trip.bin\n");
            return;
        }
        drivers.blk_flush();
        kprintf("[EXT2] [PASS] triple-indirect sparse /mnt/trip.bin\n");
    }
}
