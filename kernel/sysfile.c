package kernel;
import drivers;
import fs;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * File-oriented system calls: open / close / read / write / lseek.
 *
 * User pointers are validated against the caller's VMAs (as_user_range_ok,
 * VMA-level coverage; demand paging resolves touched holes transparently)
 * and only ever dereferenced with the process CR3 installed.
 * ------------------------------------------------------------------------- */

enum {
    PATH_KMAX  = 128,
    IO_CHUNK   = 4096,
    DENT_CHUNK = 1920,             /* 40 x 48-byte dirent records */
    FD_BAD     = 0xFFFFFFFF,

    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,

    INO_DIR_K = 2,                 /* fs INO_DIR numeric contract */

    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2
};

/* sched.c (same package) */
extern u64 sched_current_as_h(void);
extern u32 sched_fd_get(u32 idx);
extern void sched_fd_set(u32 idx, u32 fh);
extern u32 sched_fd_alloc(void);
extern const char *sched_cwd(void);
extern void sched_cwd_set(const char *kpath);

/* kernel/tty.c (canonical serial input) */
extern u64 tty_console_read(u64 ubuf, u64 len);

static u32 kstr_len(const char *s) {
    u32 n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/* Path canonicalization: fold "/" separators, "." and ".." into a clean
 * absolute path. Absolute raw paths restart at "/"; relative ones extend the
 * process cwd. Purely lexical - existence is checked by the caller via VFS. */
static u32 canon_push(char *buf, const char *comp, u32 n, u32 cap) {
    u32 l = kstr_len(buf);
    u32 base;
    u32 i;
    if (l == 1 && buf[0] == '/') {
        if ((u32)1 + n + 1 > cap) {
            return 0;
        }
        base = 1;
    } else {
        if (l + 1 + n + 1 > cap) {
            return 0;
        }
        buf[l] = '/';
        base = l + 1;
    }
    for (i = 0; i < n; i++) {
        buf[base + i] = comp[i];
    }
    buf[base + n] = 0;
    return 1;
}

static void canon_pop(char *buf) {
    u32 l = kstr_len(buf);
    if (l == 1) {
        return;                        /* root's parent is root */
    }
    while (l > 1 && buf[l - 1] != '/') {
        l--;
    }
    if (l == 1) {
        buf[1] = 0;
    } else {
        buf[l - 1] = 0;
    }
}

/* Returns 1 on success, 0 if the result would overflow the buffer. */
static u32 canon_path(const char *cwd, const char *raw, char *out, u32 cap) {
    u32 i = 0;
    if (cap < 2) {
        return 0;
    }
    if (raw[0] == '/') {
        out[0] = '/';
        out[1] = 0;
    } else {
        u32 l = kstr_len(cwd);
        u32 k;
        if (l + 1 > cap) {
            return 0;
        }
        for (k = 0; k <= l; k++) {
            out[k] = cwd[k];
        }
    }
    while (raw[i] != 0) {
        u32 start;
        u32 n;
        while (raw[i] == '/') {
            i++;
        }
        if (raw[i] == 0) {
            break;
        }
        start = i;
        n = 0;
        while (raw[i] != 0 && raw[i] != '/') {
            i++;
            n++;
            if (n >= PATH_KMAX) {
                return 0;
            }
        }
        if (n == 1 && raw[start] == '.') {
            continue;
        }
        if (n == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            canon_pop(out);
            continue;
        }
        if (!canon_push(out, &raw[start], n, cap)) {
            return 0;                    /* resulting path too long */
        }
    }
    return 1;
}

/* Copy a NUL-terminated string from user space with PER-PAGE VMA validation.
 * A fixed window cannot be used: argv strings may sit at the very top of the
 * demand stack where (addr + window) crosses the VMA end even though the
 * string itself is fully inside. Touched non-present pages demand-fault in. */
u32 copy_user_cstring(char *dst, u64 uva, u32 maxlen) {
    u64 as = sched_current_as_h();
    volatile char *p;
    u32 i = 0;
    if (as == 0) {
        return 0;
    }
    if (!mem.as_user_range_ok(as, uva & ~0xFFFULL, 4096)) {
        return 0;
    }
    p = (volatile char *)uva;
    while (i < maxlen) {
        char c = p[i];
        if (c == 0) {
            dst[i] = 0;
            return 1;
        }
        dst[i] = c;
        i++;
        if (((uva + i) & 0xFFF) == 0) {
            if (!mem.as_user_range_ok(as, (uva + i) & ~0xFFFULL, 4096)) {
                return 0;
            }
        }
    }
    return 0;                        /* no NUL within maxlen */
}

/* Copy a NUL-terminated path from user space and resolve it against the
 * per-process cwd into a canonical absolute kernel path. */
u32 copy_user_path(char *dst, u64 upath) {
    char raw[PATH_KMAX];
    if (!copy_user_cstring(raw, upath, PATH_KMAX - 1)) {
        return 0;
    }
    return canon_path(sched_cwd(), raw, dst, PATH_KMAX);
}

u64 sys_open(u64 upath, u64 flags, u64 mode) {
    char path[PATH_KMAX];
    u32 fh;
    u32 fd;
    (void)mode;
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    fh = fs.fs_file_open(path, (u32)flags);
    if (fh == 0) {
        return (u64)(-1);
    }
    fd = sched_fd_alloc();
    if (fd == FD_BAD) {
        fs.fs_close(fh);
        return (u64)(-1);
    }
    sched_fd_set(fd, fh);
    return (u64)fd;
}

u64 sys_close(u64 fd) {
    u32 fh = sched_fd_get((u32)fd);
    u32 ok;
    if (fh == FD_BAD) {
        return (u64)(-1);
    }
    ok = fs.fs_close(fh);
    sched_fd_set((u32)fd, FD_BAD);
    return ok ? 0 : (u64)(-1);
}

u64 sys_lseek(u64 fd, u64 off, u64 whence) {
    u32 fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD) {
        return (u64)(-1);
    }
    return fs.fs_lseek_h(fh, off, (u32)whence);
}

u64 sys_read_fd(u64 fd, u64 buf, u64 len) {
    u64 as = sched_current_as_h();
    u32 fh;
    u64 done = 0;
    char kbuf[IO_CHUNK];

    fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD || as == 0) {
        return (u64)(-1);
    }

    /* fd 0: serial console with canonical line discipline. */
    if (fs.fs_file_is_console(fh)) {
        return tty_console_read(buf, len);
    }
    if (fs.fs_file_is_dir(fh)) {
        return (u64)(-1);             /* directories use getdents64 */
    }

    while (done < len) {
        u64 want = len - done;
        u64 n;
        u64 i;
        if (want > IO_CHUNK) {
            want = IO_CHUNK;
        }
        n = fs.fs_read_h(fh, kbuf, want);
        if (n == (u64)(-1)) {
            return done != 0 ? done : (u64)(-1);
        }
        if (n == 0) {
            break;                   /* EOF */
        }
        if (!mem.as_user_range_ok(as, buf + done, n)) {
            return done != 0 ? done : (u64)(-1);
        }
        {
            volatile char *u = (volatile char *)(buf + done);
            for (i = 0; i < n; i++) {
                u[i] = kbuf[i];
            }
        }
        done += n;
    }
    return done;
}

u64 sys_write_fd(u64 fd, u64 buf, u64 len) {
    u64 as = sched_current_as_h();
    u32 fh;
    u64 done = 0;
    char kbuf[IO_CHUNK];

    fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD || as == 0) {
        return (u64)(-1);
    }

    /* fd 1/2: serial console sink. Validate the whole user range first, then
     * stream byte by byte (uart_putc performs the \n -> \r\n translation). */
    if (fs.fs_file_is_console(fh)) {
        volatile char *p;
        u64 i;
        if (!mem.as_user_range_ok(as, buf, len)) {
            return (u64)(-1);
        }
        p = (volatile char *)buf;
        for (i = 0; i < len; i++) {
            drivers.uart_putc(p[i]);
        }
        return len;
    }
    if (fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }

    while (done < len) {
        u64 want = len - done;
        u64 n;
        u64 i;
        if (want > IO_CHUNK) {
            want = IO_CHUNK;
        }
        if (!mem.as_user_range_ok(as, buf + done, want)) {
            return done != 0 ? done : (u64)(-1);
        }
        {
            volatile char *u = (volatile char *)(buf + done);
            for (i = 0; i < want; i++) {
                kbuf[i] = u[i];
            }
        }
        n = fs.fs_write_h(fh, kbuf, want);
        if (n == (u64)(-1)) {
            return done != 0 ? done : (u64)(-1);
        }
        done += n;
        if (n < want) {
            break;
        }
    }
    return done;
}

/* ---------------------------------------------------------------------------
 * Directory and working-directory syscalls
 * ------------------------------------------------------------------------- */

u64 sys_mkdir(u64 upath) {
    char path[PATH_KMAX];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    return fs.fs_mkdir(path) != 0 ? 0 : (u64)(-1);
}

u64 sys_chdir(u64 upath) {
    char path[PATH_KMAX];
    u32 ino;
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    ino = fs.fs_lookup(path);
    if (fs.fs_ino_kind(ino) != INO_DIR_K) {
        return (u64)(-1);
    }
    sched_cwd_set(path);
    return 0;
}

/* Returns the buffer address on success (Linux convention), (u64)-1 when the
 * buffer is too small or invalid. */
u64 sys_getcwd(u64 ubuf, u64 len) {
    const char *cwd = sched_cwd();
    u32 n = kstr_len(cwd) + 1;
    u64 as = sched_current_as_h();
    u32 i;
    if (as == 0 || len < n || !mem.as_user_range_ok(as, ubuf, n)) {
        return (u64)(-1);
    }
    {
        volatile char *u = (volatile char *)ubuf;
        for (i = 0; i < n; i++) {
            u[i] = cwd[i];
        }
    }
    return ubuf;
}

/* getdents64: stream fixed 48-byte records from a directory descriptor.
 * The fs layer owns the per-file enumeration offset. */
u64 sys_getdents64(u64 fd, u64 ubuf, u64 len) {
    u64 as = sched_current_as_h();
    u32 fh = sched_fd_get((u32)fd);
    u64 done = 0;
    char kbuf[DENT_CHUNK];

    if (fh == FD_BAD || as == 0) {
        return (u64)(-1);
    }
    if (!fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }
    while (done < len) {
        u64 want = len - done;
        u64 n;
        u64 i;
        if (want > DENT_CHUNK) {
            want = DENT_CHUNK;
        }
        n = fs.fs_getdents_h(fh, kbuf, want);
        if (n == (u64)(-1)) {
            return done != 0 ? done : (u64)(-1);
        }
        if (n == 0) {
            break;                       /* end of directory */
        }
        if (!mem.as_user_range_ok(as, ubuf + done, n)) {
            return done != 0 ? done : (u64)(-1);
        }
        {
            volatile char *u = (volatile char *)(ubuf + done);
            for (i = 0; i < n; i++) {
                u[i] = kbuf[i];
            }
        }
        done += n;
    }
    return done;
}
