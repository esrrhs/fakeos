#!/usr/bin/env python3
"""Create a minimal Ext2 filesystem image for fakeos VirtIO-blk tests.

Layout (1 KiB blocks, multi block-group when image is large enough):
  block 0          : boot block (zeros; unused)
  block 1          : superblock
  block 2..        : block group descriptor table
  per group        : block bitmap, inode bitmap, inode table, (group0: root)

Bitmap indexing matches fakeos fs/ext2.c:
  groups_count==1 -> absolute bit = block number
  groups_count>1  -> group-relative bit = block - group_base

Usage: mkext2.py <out.img> [megs=4]
"""
from __future__ import annotations
import math
import struct
import sys

BLOCK = 1024
INODE_SIZE = 128
FIRST_INO = 11  # first non-reserved inode number


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: mkext2.py <out.img> [megs]", file=sys.stderr)
        return 2
    out = sys.argv[1]
    megs = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    total_bytes = megs * 1024 * 1024
    blocks_count = total_bytes // BLOCK
    first_data_block = 1  # for 1K blocks, SB is in block 1

    # Prefer multi-bg once the image can hold two groups with room for data.
    # 2048 blocks/group => 2 MiB/group at 1K; a 4 MiB image yields 2 groups.
    if blocks_count >= 4096:
        blocks_per_group = 2048
    else:
        blocks_per_group = blocks_count - first_data_block
        if blocks_per_group < 1:
            blocks_per_group = blocks_count

    groups_count = (
        blocks_count - first_data_block + blocks_per_group - 1
    ) // blocks_per_group
    if groups_count < 1:
        groups_count = 1

    inodes_per_group = 64
    inodes_count = inodes_per_group * groups_count
    inode_table_blocks = (inodes_per_group * INODE_SIZE + BLOCK - 1) // BLOCK
    descs_per_block = BLOCK // 32
    bgdt_blocks = (groups_count + descs_per_block - 1) // descs_per_block

    img = bytearray(total_bytes)

    def put(off: int, data: bytes) -> None:
        img[off : off + len(data)] = data

    def group_base(g: int) -> int:
        return first_data_block + g * blocks_per_group

    def group_lim(g: int) -> int:
        lim = group_base(g) + blocks_per_group
        return min(lim, blocks_count)

    # Per-group metadata block numbers.
    meta = []
    for g in range(groups_count):
        base = group_base(g)
        if g == 0:
            # SB at 1, BGDT at 2.., then bitmaps + itable.
            bg_block = first_data_block + 1  # 2
            bmap = bg_block + bgdt_blocks
            imap = bmap + 1
            itable = imap + 1
            root_data = itable + inode_table_blocks
            first_free = root_data + 1
        else:
            bmap = base
            imap = bmap + 1
            itable = imap + 1
            root_data = None
            first_free = itable + inode_table_blocks
        meta.append(
            {
                "base": base,
                "lim": group_lim(g),
                "bmap": bmap,
                "imap": imap,
                "itable": itable,
                "root_data": root_data,
                "first_free": first_free,
            }
        )

    # Used block sets per group (absolute block numbers).
    used: list[set[int]] = [set() for _ in range(groups_count)]
    # Group 0 structural.
    used[0].update(range(0, meta[0]["first_free"]))  # includes boot+SB+...
    # Drop boot block 0 from bitmap consideration for multi-bg relative mode;
    # still mark it used in absolute single-group mode via the same set.
    for g in range(1, groups_count):
        used[g].update(range(meta[g]["bmap"], meta[g]["first_free"]))

    free_blocks = 0
    for g in range(groups_count):
        for b in range(meta[g]["base"], meta[g]["lim"]):
            if b not in used[g]:
                free_blocks += 1
    # Block 0 is before first_data_block; not counted in free_blocks walk above
    # when base starts at 1. Good.

    free_inodes = inodes_count - (FIRST_INO - 1) - 1  # reserved 1..10 + root(2)
    # inodes 1..10 reserved marked used; inode 2 is root (inside 1..10).
    # free = total - 10 (all reserved including root)
    free_inodes = inodes_count - 10

    # ---- superblock at byte 1024 (block 1) ----
    sb = bytearray(BLOCK)
    put_sb = lambda o, d: sb.__setitem__(slice(o, o + len(d)), d)
    put_sb(0, u32(inodes_count))
    put_sb(4, u32(blocks_count))
    put_sb(8, u32(0))  # r_blocks
    put_sb(12, u32(free_blocks))
    put_sb(16, u32(free_inodes))
    put_sb(20, u32(first_data_block))
    put_sb(24, u32(0))  # log_block_size = 0 -> 1K
    put_sb(28, u32(0))  # log_frag_size
    put_sb(32, u32(blocks_per_group))
    put_sb(36, u32(blocks_per_group))  # frags_per_group
    put_sb(40, u32(inodes_per_group))
    put_sb(44, u32(1))  # mtime
    put_sb(48, u32(1))  # wtime
    put_sb(54, u16(1))  # mnt_count
    put_sb(56, u16(0xEF53))
    put_sb(58, u16(1))  # state = valid
    put_sb(62, u16(1))  # errors
    put_sb(76, u32(1))  # rev_level dynamic
    put_sb(84, u32(FIRST_INO))
    put_sb(88, u16(INODE_SIZE))
    put_sb(92, u32(0))  # feature_compat
    put_sb(96, u32(0x2))  # feature_incompat: filetype
    put_sb(100, u32(0))  # feature_ro_compat
    put(BLOCK, bytes(sb))

    # ---- block group descriptor table ----
    bgdt = bytearray(bgdt_blocks * BLOCK)
    for g in range(groups_count):
        m = meta[g]
        g_free_blocks = sum(
            1 for b in range(m["base"], m["lim"]) if b not in used[g]
        )
        if g == 0:
            g_free_inodes = inodes_per_group - 10
            used_dirs = 1
        else:
            g_free_inodes = inodes_per_group
            used_dirs = 0
        off = g * 32
        bgdt[off : off + 4] = u32(m["bmap"])
        bgdt[off + 4 : off + 8] = u32(m["imap"])
        bgdt[off + 8 : off + 12] = u32(m["itable"])
        bgdt[off + 12 : off + 14] = u16(g_free_blocks)
        bgdt[off + 14 : off + 16] = u16(g_free_inodes)
        bgdt[off + 16 : off + 18] = u16(used_dirs)
    put(2 * BLOCK, bytes(bgdt))

    # ---- bitmaps + inode tables ----
    for g in range(groups_count):
        m = meta[g]
        bmap = bytearray(BLOCK)
        for b in sorted(used[g]):
            if groups_count == 1:
                bit = b
            else:
                if b < m["base"] or b >= m["lim"]:
                    # boot block 0 for g0: skip relative bitmap
                    if b == 0 and g == 0:
                        continue
                    continue
                bit = b - m["base"]
            if 0 <= bit < BLOCK * 8:
                bmap[bit // 8] |= 1 << (bit % 8)
        # For single-group absolute mode also mark block 0 used.
        if groups_count == 1:
            bmap[0] |= 1
        put(m["bmap"] * BLOCK, bytes(bmap))

        imap = bytearray(BLOCK)
        if g == 0:
            for i in range(0, 10):  # inodes 1..10
                imap[i // 8] |= 1 << (i % 8)
        put(m["imap"] * BLOCK, bytes(imap))

        itable = bytearray(inode_table_blocks * BLOCK)
        if g == 0:
            root = bytearray(INODE_SIZE)
            root[0:2] = u16(0x41ED)  # S_IFDIR | 0755
            root[4:8] = u32(BLOCK)  # size = one block
            root[26:28] = u16(2)  # links (. and ..)
            root[28:32] = u32(BLOCK // 512)  # i_blocks
            root[40:44] = u32(m["root_data"])  # i_block[0]
            off = (2 - 1) * INODE_SIZE
            itable[off : off + INODE_SIZE] = root
        put(m["itable"] * BLOCK, bytes(itable))

    # ---- root directory data block ----
    root_data = meta[0]["root_data"]
    dent = bytearray(BLOCK)
    dent[0:4] = u32(2)
    dent[4:6] = u16(12)
    dent[6] = 1
    dent[7] = 2  # DIR
    dent[8] = ord(".")
    dent[12:16] = u32(2)
    dent[16:18] = u16(BLOCK - 12)
    dent[18] = 2
    dent[19] = 2
    dent[20] = ord(".")
    dent[21] = ord(".")
    put(root_data * BLOCK, bytes(dent))

    with open(out, "wb") as f:
        f.write(img)
    print(
        f"wrote {out}: {megs}MiB, {blocks_count}x{BLOCK}B blocks, "
        f"{groups_count} groups x {blocks_per_group} bpg, "
        f"{inodes_count} inodes, root_data={root_data}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
