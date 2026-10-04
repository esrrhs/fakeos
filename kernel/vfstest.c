package kernel;
import fs;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * VFS / Ramfs in-kernel self-test. Runs once at boot after the rootfs image
 * has been published; it works under a throwaway /selftest tree so it never
 * clashes with real image paths.
 * ------------------------------------------------------------------------- */

/* fs package enum constants are not visible across packages; the numeric
 * contracts are duplicated here on purpose. */
enum {
    T_O_RDONLY = 0,
    T_O_RDWR   = 2,
    T_O_APPEND = 0x400,
    T_SEEK_SET = 0,
    T_SEEK_END = 2,
    T_DENT_RECLEN = 48,
    T_FILE_MAX = 4194304
};

/* Spelled as a string literal rather than a char initialiser list: this is
 * the only kernel source that used `static const char x[] = { 'a', ... }`,
 * and fakecc emits different code for that form (the in-OS compiler and the
 * host compiler disagreed by 5 bytes on this file alone). A const pointer to
 * a literal keeps the read-only intent -- the test writes through a char*
 * on purpose, to prove a static file rejects writes -- without depending on
 * that initialiser form. */
static const char *static_msg = "ramfs-static!!";

static u32 vfs_fails;

static void vfs_expect(u32 cond, const char *what) {
    if (cond) {
        kprintf("        [ok] %s\n", what);
    } else {
        kprintf("        [FAIL] %s\n", what);
        vfs_fails++;
    }
}

static u64 get_u64(const char *p) {
    u64 v = 0;
    u32 i;
    for (i = 0; i < 8; i++) {
        v |= ((u64)(u8)p[i]) << (i * 8);
    }
    return v;
}

static u32 rec_name_is(const char *r, const char *name) {
    u32 i = 0;
    while (name[i] != 0 && i < 29) {
        if (r[19 + i] != name[i]) {
            return 0;
        }
        i++;
    }
    return r[19 + i] == 0 && name[i] == 0;
}

void vfs_selftest(void) {
    char buf[256];
    char recs[T_DENT_RECLEN * 4];
    u32 fh;
    u32 ino;
    u64 n;
    u64 i;
    u32 saw_etc;
    u32 saw_dyn;

    vfs_fails = 0;
    kprintf("\n[FS] VFS + Ramfs Self-Test:\n");

    /* --- path resolution ------------------------------------------------ */
    vfs_expect(fs.fs_lookup("/") == 1, "root directory lookup");

    vfs_expect(fs.fs_publish_file("/selftest/etc/hello.txt",
                                  (u64)static_msg, 14) == 1,
               "publish static file creates intermediate dirs");
    ino = fs.fs_lookup("/selftest/etc/hello.txt");
    vfs_expect(ino != 0, "published file lookup");
    vfs_expect(fs.fs_ino_kind(ino) == 1 /* INO_FILE */, "inode kind is file");
    vfs_expect(fs.fs_ino_size(ino) == 14, "static inode size 14");

    vfs_expect(fs.fs_lookup("/selftest/./etc//hello.txt") == ino,
               "dot component and doubled slashes folded");
    vfs_expect(fs.fs_lookup("/selftest/etc/../etc/hello.txt") == ino,
               "dot-dot component resolved");
    vfs_expect(fs.fs_lookup("/selftest/..") == 1, "dot-dot from root tree reaches root");

    vfs_expect(fs.fs_lookup("/selftest/nope/missing") == 0,
               "negative: missing intermediate component");
    vfs_expect(fs.fs_lookup("/selftest/etc/hello.txt/sub") == 0,
               "negative: component through a file");
    vfs_expect(fs.fs_publish_file("/selftest/etc/hello.txt",
                                  (u64)static_msg, 14) == 0,
               "negative: republish same path rejected");

    /* static files are immutable */
    vfs_expect(fs.fs_file_open("/selftest/etc/hello.txt", T_O_RDWR) == 0,
               "negative: static file cannot be opened writable");
    fh = fs.fs_file_open("/selftest/etc/hello.txt", T_O_RDONLY);
    vfs_expect(fh != 0, "static file opened read-only");
    n = fs.fs_read_h(fh, buf, sizeof(buf));
    vfs_expect(n == 14, "static read returns 14 bytes");
    {
        u32 same = (n == 14);
        for (i = 0; i < 14; i++) {
            if (buf[i] != static_msg[i]) {
                same = 0;
            }
        }
        vfs_expect(same, "static content byte-identical");
    }
    vfs_expect(fs.fs_read_h(fh, buf, sizeof(buf)) == 0, "read at EOF returns 0");
    vfs_expect(fs.fs_write_h(fh, buf, 1) == (u64)(-1),
               "negative: write through read-only fh rejected");
    fs.fs_close(fh);

    /* --- dynamic file create / write / read / seek / growth ------------- */
    vfs_expect(fs.fs_create("/selftest/dyn") != 0, "dynamic file created");
    vfs_expect(fs.fs_create("/selftest/dyn") == 0,
               "negative: recreate same name rejected");
    fh = fs.fs_file_open("/selftest/dyn", T_O_RDWR);
    vfs_expect(fh != 0, "dynamic file opened RDWR");

    /* 6400 bytes in 100 x 64: crosses the 4096 -> 8192 growth step. Each
     * block's first byte is its block index, remaining bytes a fill. */
    for (i = 0; i < 100; i++) {
        u32 j;
        buf[0] = (char)(i & 0xFF);
        for (j = 1; j < 64; j++) {
            buf[j] = (char)('A' + (i & 7));
        }
        vfs_expect(fs.fs_write_h(fh, buf, 64) == 64, "write block (growth)");
    }
    vfs_expect(fs.fs_lseek_h(fh, 0, T_SEEK_SET) == 0, "lseek back to 0");
    {
        u32 good = 1;
        for (i = 0; i < 100; i++) {
            n = fs.fs_read_h(fh, buf, 64);
            if (n != 64 || (u8)buf[0] != (u8)(i & 0xFF)) {
                good = 0;
            }
        }
        vfs_expect(good, "6400 bytes round-trip across growth boundary");
    }

    /* Append at EOF. */
    vfs_expect(fs.fs_lseek_h(fh, 0, T_SEEK_END) == 6400, "lseek END at 6400");
    for (i = 0; i < 10; i++) {
        buf[i] = (char)('0' + i);
    }
    vfs_expect(fs.fs_write_h(fh, buf, 10) == 10, "append write");
    vfs_expect(fs.fs_lseek_h(fh, 6400, T_SEEK_SET) == 6400, "seek to appended tail");
    vfs_expect(fs.fs_read_h(fh, buf, 10) == 10, "read appended tail");
    {
        u32 good = 1;
        for (i = 0; i < 10; i++) {
            if (buf[i] != (char)('0' + i)) {
                good = 0;
            }
        }
        vfs_expect(good, "appended content matches");
    }

    /* Growth ceiling: dynamic buffers double through kmalloc; the buddy
     * allocator's largest block is 4 MiB, so a write at offset 4 MiB cannot
     * be backed and is rejected without changing the file. */
    vfs_expect(fs.fs_lseek_h(fh, T_FILE_MAX, T_SEEK_SET) == T_FILE_MAX,
               "seek near file ceiling");
    vfs_expect(fs.fs_write_h(fh, buf, 1) == (u64)(-1),
               "negative: write beyond 4 MiB rejected without growth");
    ino = fs.fs_lookup("/selftest/dyn");
    vfs_expect(fs.fs_ino_size(ino) == 6410, "file size untouched after failed write");
    fs.fs_close(fh);

    /* O_APPEND positions writes at EOF. */
    fh = fs.fs_file_open("/selftest/dyn", T_O_RDWR | T_O_APPEND);
    vfs_expect(fh != 0 && fs.fs_write_h(fh, "Z", 1) == 1, "O_APPEND write");
    fs.fs_close(fh);
    fh = fs.fs_file_open("/selftest/dyn", T_O_RDONLY);
    vfs_expect(fs.fs_lseek_h(fh, 6410, T_SEEK_SET) == 6410 &&
               fs.fs_read_h(fh, buf, 1) == 1 && buf[0] == 'Z',
               "O_APPEND landed at EOF");
    fs.fs_close(fh);

    /* --- directories ---------------------------------------------------- */
    vfs_expect(fs.fs_mkdir("/selftest/sub") != 0, "mkdir");
    vfs_expect(fs.fs_mkdir("/selftest/sub") == 0,
               "negative: duplicate mkdir rejected");
    vfs_expect(fs.fs_mkdir("/selftest/dyn/under") == 0,
               "negative: mkdir under a file rejected");

    fh = fs.fs_file_open("/selftest", T_O_RDONLY);
    vfs_expect(fh != 0, "directory opened for getdents");
    n = fs.fs_getdents_h(fh, recs, sizeof(recs));
    vfs_expect(n >= T_DENT_RECLEN * 3 && n <= sizeof(recs),
               "getdents returns at least 3 entries");
    saw_etc = 0;
    saw_dyn = 0;
    if (n <= sizeof(recs)) {
        u32 k;
        for (k = 0; k + T_DENT_RECLEN <= (u32)n; k += T_DENT_RECLEN) {
            char *r = &recs[k];
            u64 dino = get_u64(r);
            u64 doff = get_u64(r + 8);
            vfs_expect(dino != 0 && doff == (u64)(k / T_DENT_RECLEN + 1),
                       "dirent ino nonzero and offsets sequential");
            if (rec_name_is(r, "etc")) {
                saw_etc = 1;
            }
            if (rec_name_is(r, "dyn")) {
                saw_dyn = 1;
            }
        }
    }
    vfs_expect(saw_etc, "directory listing contains 'etc'");
    vfs_expect(saw_dyn, "directory listing contains 'dyn'");
    vfs_expect(fs.fs_getdents_h(fh, recs, sizeof(recs)) == 0,
               "getdents at end returns 0");
    vfs_expect(fs.fs_read_h(fh, buf, 1) == (u64)(-1),
               "negative: read on directory rejected");
    fs.fs_close(fh);

    /* --- bad handles ---------------------------------------------------- */
    vfs_expect(fs.fs_read_h(60, buf, 1) == (u64)(-1),
               "negative: read through invalid handle");
    vfs_expect(fs.fs_close(60) == 0, "negative: close invalid handle");
    vfs_expect(fs.fs_lseek_h(0, 0, T_SEEK_SET) == (u64)(-1),
               "negative: seek invalid handle");

    /* --- summary -------------------------------------------------------- */
    if (vfs_fails == 0) {
        kprintf("      [PASS] VFS path resolution\n");
        kprintf("      [PASS] Ramfs file create/read/write/seek\n");
        kprintf("      [PASS] Ramfs directory listing\n");
    } else {
        kprintf("      [FAIL] VFS/Ramfs self-test: %u failed checks\n",
                (u64)vfs_fails);
    }
}
