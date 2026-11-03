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
    CWD_LEN    = 64,
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

/* kernel/pipe.c */
extern u32 pipe_fh_is(u32 fh);
extern u32 pipe_alloc(u32 *rfh, u32 *wfh);
extern void pipe_retain(u32 fh);
extern u32 pipe_close(u32 fh);
extern u64 pipe_read(u32 fh, char *kbuf, u64 len);
extern u64 pipe_write(u32 fh, const char *kbuf, u64 len);


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
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()) {
            return (u64)(-1);
        }
        fh = fs.ext2_open(path, (u32)flags);
    } else {
        fh = fs.fs_file_open(path, (u32)flags);
    }
    if (fh == 0) {
        return (u64)(-1);
    }
    fd = sched_fd_alloc();
    if (fd == FD_BAD) {
        if (fs.ext2_fh_is(fh)) {
            fs.ext2_close(fh);
        } else {
            fs.fs_close(fh);
        }
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
    if (fs.ext2_fh_is(fh)) {
        ok = fs.ext2_close(fh);
    } else if (pipe_fh_is(fh)) {
        ok = pipe_close(fh);
    } else {
        ok = fs.fs_close(fh);
    }
    sched_fd_set((u32)fd, FD_BAD);
    return ok ? 0 : (u64)(-1);
}

/* dup2(oldfd, newfd): make newfd refer to the same open-file as oldfd. */
u64 sys_dup2(u64 oldfd, u64 newfd) {
    u32 ofh;
    u32 nfh;
    if (oldfd >= 16 || newfd >= 16) {
        return (u64)(-1);
    }
    ofh = sched_fd_get((u32)oldfd);
    if (ofh == FD_BAD) {
        return (u64)(-1);
    }
    if (oldfd == newfd) {
        return newfd;
    }
    nfh = sched_fd_get((u32)newfd);
    if (nfh != FD_BAD) {
        if (fs.ext2_fh_is(nfh)) {
            fs.ext2_close(nfh);
        } else if (pipe_fh_is(nfh)) {
            pipe_close(nfh);
        } else {
            fs.fs_close(nfh);
        }
        sched_fd_set((u32)newfd, FD_BAD);
    }
    if (fs.ext2_fh_is(ofh)) {
        fs.ext2_file_retain(ofh);
    } else if (pipe_fh_is(ofh)) {
        pipe_retain(ofh);
    } else {
        fs.fs_file_retain(ofh);
    }
    sched_fd_set((u32)newfd, ofh);
    return newfd;
}

u64 sys_lseek(u64 fd, u64 off, u64 whence) {
    u32 fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD) {
        return (u64)(-1);
    }
    if (pipe_fh_is(fh)) {
        return (u64)(-1);
    }
    if (fs.ext2_fh_is(fh)) {
        return fs.ext2_lseek(fh, off, (u32)whence);
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
    if (!fs.ext2_fh_is(fh) && !pipe_fh_is(fh) && fs.fs_file_is_console(fh)) {
        return tty_console_read(buf, len);
    }
    if (pipe_fh_is(fh)) {
        /* Pipe reads may block; copy one chunk at a time into user memory. */
        while (done < len) {
            u64 want = len - done;
            u64 n;
            u64 i;
            if (want > IO_CHUNK) {
                want = IO_CHUNK;
            }
            n = pipe_read(fh, kbuf, want);
            if (n == (u64)(-1)) {
                return done != 0 ? done : (u64)(-1);
            }
            if (n == 0) {
                break;
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
            /* Pipes return whatever is available rather than filling len. */
            break;
        }
        return done;
    }
    if (fs.ext2_fh_is(fh) ? fs.ext2_file_is_dir(fh) : fs.fs_file_is_dir(fh)) {
        return (u64)(-1);             /* directories use getdents64 */
    }

    while (done < len) {
        u64 want = len - done;
        u64 n;
        u64 i;
        if (want > IO_CHUNK) {
            want = IO_CHUNK;
        }
        n = fs.ext2_fh_is(fh) ? fs.ext2_read(fh, kbuf, want) : fs.fs_read_h(fh, kbuf, want);
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
    if (!fs.ext2_fh_is(fh) && !pipe_fh_is(fh) && fs.fs_file_is_console(fh)) {
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
    if (pipe_fh_is(fh)) {
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
            n = pipe_write(fh, kbuf, want);
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
    if (fs.ext2_fh_is(fh) ? fs.ext2_file_is_dir(fh) : fs.fs_file_is_dir(fh)) {
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
        n = fs.ext2_fh_is(fh) ? fs.ext2_write(fh, kbuf, want) : fs.fs_write_h(fh, kbuf, want);
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


/* pipe(pipefd[2]): create a unidirectional pipe; pipefd[0] is the read end
 * and pipefd[1] the write end (Linux int[2] layout). */
u64 sys_pipe(u64 upipefd) {
    u64 as = sched_current_as_h();
    u32 rfh;
    u32 wfh;
    u32 rfd;
    u32 wfd;
    volatile u32 *u;
    if (as == 0 || !mem.as_user_range_ok(as, upipefd, 8)) {
        return (u64)(-1);
    }
    if (!pipe_alloc(&rfh, &wfh)) {
        return (u64)(-1);
    }
    rfd = sched_fd_alloc();
    if (rfd == FD_BAD) {
        pipe_close(rfh);
        pipe_close(wfh);
        return (u64)(-1);
    }
    sched_fd_set(rfd, rfh);
    wfd = sched_fd_alloc();
    if (wfd == FD_BAD) {
        sched_fd_set(rfd, FD_BAD);
        pipe_close(rfh);
        pipe_close(wfh);
        return (u64)(-1);
    }
    sched_fd_set(wfd, wfh);
    u = (volatile u32 *)upipefd;
    u[0] = rfd;
    u[1] = wfd;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Directory and working-directory syscalls
 * ------------------------------------------------------------------------- */

u64 sys_mkdir(u64 upath) {
    char path[PATH_KMAX];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        return fs.ext2_mounted() && fs.ext2_mkdir(path) ? 0 : (u64)(-1);
    }
    return fs.fs_mkdir(path) != 0 ? 0 : (u64)(-1);
}

u64 sys_unlink(u64 upath) {
    char path[PATH_KMAX];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        return fs.ext2_mounted() && fs.ext2_unlink(path) ? 0 : (u64)(-1);
    }
    return fs.fs_unlink(path) ? 0 : (u64)(-1);
}

u64 sys_rmdir(u64 upath) {
    char path[PATH_KMAX];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        return fs.ext2_mounted() && fs.ext2_rmdir(path) ? 0 : (u64)(-1);
    }
    return fs.fs_rmdir(path) ? 0 : (u64)(-1);
}

u64 sys_chdir(u64 upath) {
    char path[PATH_KMAX];
    u32 ino;
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        u32 fh;
        if (!fs.ext2_mounted()) {
            return (u64)(-1);
        }
        fh = fs.ext2_open(path, O_RDONLY);
        if (fh == 0 || !fs.ext2_file_is_dir(fh)) {
            if (fh) fs.ext2_close(fh);
            return (u64)(-1);
        }
        fs.ext2_close(fh);
    } else {
        ino = fs.fs_lookup(path);
        if (fs.fs_ino_kind(ino) != INO_DIR_K) {
            return (u64)(-1);
        }
    }
    if (kstr_len(path) >= CWD_LEN) {
        return (u64)(-1);            /* would silently truncate the cwd */
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
    if (pipe_fh_is(fh)) {
        return (u64)(-1);
    }
    if (fs.ext2_fh_is(fh) ? !fs.ext2_file_is_dir(fh) : !fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }
    while (done < len) {
        u64 want = len - done;
        u64 n;
        u64 i;
        if (want > DENT_CHUNK) {
            want = DENT_CHUNK;
        }
        n = fs.ext2_fh_is(fh) ? fs.ext2_getdents(fh, kbuf, want)
                           : fs.fs_getdents_h(fh, kbuf, want);
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

u64 sys_sync(void) {
    if (fs.ext2_mounted()) {
        fs.ext2_sync();
    }
    return 0;
}
