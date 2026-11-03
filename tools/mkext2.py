#!/usr/bin/env python3
"""Create a minimal Ext2 filesystem image for fakeos VirtIO-blk tests.

Layout (1 KiB blocks, single block group), sized for `megs` megabytes:
  block 0     : boot block (zeros; unused)
  block 1     : superblock
  block 2     : block group descriptor
  block 3     : block bitmap
  block 4     : inode bitmap
  block 5..   : inode table
  next        : root directory data block

Usage: mkext2.py <out.img> [megs=4]
"""
from __future__ import annotations
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
    # Keep inode count modest but enough for daily-driver experiments.
    inodes_count = 64
    inodes_per_group = inodes_count
    blocks_per_group = blocks_count

    inode_table_blocks = (inodes_count * INODE_SIZE + BLOCK - 1) // BLOCK
    # Block assignment
    first_data_block = 1  # for 1K blocks, SB is in block 1
    bg_block = 2
    block_bitmap = 3
    inode_bitmap = 4
    inode_table = 5
    root_data = inode_table + inode_table_blocks
    first_free = root_data + 1

    img = bytearray(total_bytes)

    def put(off: int, data: bytes) -> None:
        img[off : off + len(data)] = data

    # ---- superblock at byte 1024 (block 1) ----
    sb = bytearray(BLOCK)
    put_sb = lambda o, d: sb.__setitem__(slice(o, o + len(d)), d)
    put_sb(0, u32(inodes_count))
    put_sb(4, u32(blocks_count))
    put_sb(8, u32(0))  # r_blocks
    put_sb(12, u32(blocks_count - first_free))  # free blocks (approx)
    put_sb(16, u32(inodes_count - FIRST_INO + 1 - 1))  # free inodes approx
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
    put_sb(62, u16(1))  # rev_level minor / errors
    # rev_level at 76: 1 for dynamic rev (inode_size field valid)
    put_sb(76, u32(1))
    put_sb(84, u32(FIRST_INO))
    put_sb(88, u16(INODE_SIZE))
    put_sb(92, u32(0))  # feature_compat
    put_sb(96, u32(0x2))  # feature_incompat: filetype
    put_sb(100, u32(0))  # feature_ro_compat
    put(BLOCK, bytes(sb))  # offset 1024

    # ---- block group descriptor at block 2 ----
    bg = bytearray(BLOCK)
    bg[0:4] = u32(block_bitmap)
    bg[4:8] = u32(inode_bitmap)
    bg[8:12] = u32(inode_table)
    bg[12:14] = u16(blocks_count - first_free)
    bg[14:16] = u16(inodes_count - FIRST_INO)  # free inodes (inode 1..10 reserved used?)
    bg[16:18] = u16(1)  # used_dirs = root
    put(bg_block * BLOCK, bytes(bg))

    # ---- block bitmap (block 3): mark 0..root_data used ----
    bmap = bytearray(BLOCK)
    for b in range(0, first_free):
        bmap[b // 8] |= 1 << (b % 8)
    # Also mark blocks beyond blocks_count? not present
    put(block_bitmap * BLOCK, bytes(bmap))

    # ---- inode bitmap (block 4): mark inodes 1..10 + root(2) used ----
    imap = bytearray(BLOCK)
    for i in range(0, 10):  # inodes 1..10
        imap[i // 8] |= 1 << (i % 8)
    # inode 2 is already in 1..10
    put(inode_bitmap * BLOCK, bytes(imap))

    # ---- inode table ----
    itable = bytearray(inode_table_blocks * BLOCK)

    def write_inode(ino: int, raw: bytes) -> None:
        off = (ino - 1) * INODE_SIZE
        itable[off : off + len(raw)] = raw

    # inode 2 = root directory
    root = bytearray(INODE_SIZE)
    root[0:2] = u16(0x41ED)  # S_IFDIR | 0755
    root[4:8] = u32(BLOCK)  # size = one block
    root[26:28] = u16(2)  # links (. and ..)
    root[28:32] = u32(BLOCK // 512)  # i_blocks
    root[40:44] = u32(root_data)  # i_block[0]
    write_inode(2, bytes(root))
    put(inode_table * BLOCK, bytes(itable))

    # ---- root directory data block ----
    dent = bytearray(BLOCK)
    # .
    dent[0:4] = u32(2)
    dent[4:6] = u16(12)
    dent[6] = 1
    dent[7] = 2  # DIR
    dent[8] = ord(".")
    # ..
    dent[12:16] = u32(2)
    dent[16:18] = u16(BLOCK - 12)
    dent[18] = 2
    dent[19] = 2
    dent[20] = ord(".")
    dent[21] = ord(".")
    put(root_data * BLOCK, bytes(dent))

    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: {megs}MiB, {blocks_count}x{BLOCK}B blocks, "
          f"{inodes_count} inodes, root_data={root_data}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
