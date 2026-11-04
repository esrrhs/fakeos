package kernel;
import drivers;
import fs;
import mem;
import types;

typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int64_t i64;
typedef types.int32_t s32;

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
extern u64 tty_jobctl_write_wait(void);
extern u32 tty_input_pending(void);
extern u64 sched_tick_count(void);
extern void sched_yield(void);
extern void sched_sleep(u64 chan);
extern void sched_wakeup(u64 chan);
extern u32 pipe_can_read(u32 fh);
extern u32 pipe_can_write(u32 fh);

/* Wait channel for select/poll; woken by tty/pipe/sock readiness. */
static u32 select_wait_token;

void select_kick(void) {
    sched_wakeup((u64)&select_wait_token);
}
extern u32 sched_should_eintr(void);
extern void sched_check_signals(void);
extern u32 sock_can_read(u32 fh);
extern u32 sock_can_write(u32 fh);

/* kernel/pipe.c */
extern u32 pipe_fh_is(u32 fh);
extern u32 pipe_alloc(u32 *rfh, u32 *wfh);
extern void pipe_retain(u32 fh);
extern u32 pipe_close(u32 fh);
extern u64 pipe_read(u32 fh, char *kbuf, u64 len);
extern u64 pipe_write(u32 fh, const char *kbuf, u64 len);

/* kernel/net.c sockets */
extern u32 sock_fh_is(u32 fh);
extern void sock_retain(u32 fh);
extern u32 sock_close(u32 fh);
extern u64 sys_recvfrom(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ualen);
extern u64 sys_sendto(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ulen);


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
    } else if (sock_fh_is(fh)) {
        ok = sock_close(fh);
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
        } else if (sock_fh_is(nfh)) {
            sock_close(nfh);
        } else {
            fs.fs_close(nfh);
        }
        sched_fd_set((u32)newfd, FD_BAD);
    }
    if (fs.ext2_fh_is(ofh)) {
        fs.ext2_file_retain(ofh);
    } else if (pipe_fh_is(ofh)) {
        pipe_retain(ofh);
    } else if (sock_fh_is(ofh)) {
        sock_retain(ofh);
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
    if (pipe_fh_is(fh) || sock_fh_is(fh)) {
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
    if (sock_fh_is(fh)) {
        return sys_recvfrom(fd, buf, len, 0, 0, 0);
    }
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
            if (n == (u64)(-4)) {
                return done != 0 ? done : (u64)(-4); /* EINTR */
            }
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
    if (sock_fh_is(fh)) {
        return sys_sendto(fd, buf, len, 0, 0, 0);
    }
    if (!fs.ext2_fh_is(fh) && !pipe_fh_is(fh) && fs.fs_file_is_console(fh)) {
        volatile char *p;
        u64 i;
        u64 jc;
        jc = tty_jobctl_write_wait();
        if (jc != 0) {
            return jc;
        }
        if (!mem.as_user_range_ok(as, buf, len)) {
            return (u64)(-1);
        }
        p = (volatile char *)buf;
        for (i = 0; i < len; i++) {
            drivers.uart_putc(p[i]);
            drivers.vga_ansi_putc(p[i]);
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
            if (n == (u64)(-4)) {
                return done != 0 ? done : (u64)(-4); /* EINTR */
            }
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

/* int ftruncate(fd, length): resize a regular file (Ramfs or Ext2). */
u64 sys_ftruncate(u64 fd, u64 length) {
    u32 fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD) {
        return (u64)(-1);
    }
    if (pipe_fh_is(fh) || sock_fh_is(fh)) {
        return (u64)(-1);
    }
    if (fs.ext2_fh_is(fh)) {
        return fs.ext2_ftruncate(fh, length) ? 0 : (u64)(-1);
    }
    if (fs.fs_file_is_console(fh) || fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }
    return fs.fs_ftruncate_h(fh, length) ? 0 : (u64)(-1);
}

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

u64 sys_rename(u64 uold, u64 unew) {
    char oldp[PATH_KMAX];
    char newp[PATH_KMAX];
    u32 o_ext2;
    u32 n_ext2;
    if (!copy_user_path(oldp, uold) || !copy_user_path(newp, unew)) {
        return (u64)(-1);
    }
    o_ext2 = fs.ext2_is_mnt_path(oldp);
    n_ext2 = fs.ext2_is_mnt_path(newp);
    if (o_ext2 != n_ext2) {
        return (u64)(-1);              /* EXDEV: no cross-fs rename yet */
    }
    if (o_ext2) {
        return fs.ext2_mounted() && fs.ext2_rename(oldp, newp) ? 0 : (u64)(-1);
    }
    return fs.fs_rename(oldp, newp) ? 0 : (u64)(-1);
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
    if (pipe_fh_is(fh) || sock_fh_is(fh)) {
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

/* Linux x86-64 struct stat is 144 bytes. Offsets:
 *   0 st_dev, 8 st_ino, 16 st_nlink, 24 st_mode, 28 st_uid, 32 st_gid,
 *   36 pad, 40 st_rdev, 48 st_size, 56 st_blksize, 64 st_blocks,
 *   72..112 timespecs, 120 reserved. */
enum {
    STAT_SIZE = 144,
    AT_FDCWD_U = 0xFFFFFF9C, /* -100 */
    AT_EMPTY_PATH = 0x1000,
    AT_SYMLINK_NOFOLLOW = 0x100,
    S_IFIFO = 0x1000,
    S_IFCHR = 0x2000,
    S_IFDIR = 0x4000,
    S_IFREG = 0x8000,
    S_IFLNK = 0xA000,
    S_IFSOCK = 0xC000
};

static void kstat_zero(char *k) {
    u32 i;
    for (i = 0; i < STAT_SIZE; i++) {
        k[i] = 0;
    }
}

static void kstat_put_u32(char *k, u32 off, u32 v) {
    k[off] = (char)(v & 0xFF);
    k[off + 1] = (char)((v >> 8) & 0xFF);
    k[off + 2] = (char)((v >> 16) & 0xFF);
    k[off + 3] = (char)((v >> 24) & 0xFF);
}

static void kstat_put_u64(char *k, u32 off, u64 v) {
    u32 i;
    for (i = 0; i < 8; i++) {
        k[off + i] = (char)((v >> (8 * i)) & 0xFF);
    }
}

static void kstat_fill(char *k, u64 ino, u32 mode, u64 size, u64 rdev, u32 nlink) {
    u64 blocks;
    kstat_zero(k);
    kstat_put_u64(k, 0, 1);                      /* st_dev: fake major */
    kstat_put_u64(k, 8, ino);
    kstat_put_u64(k, 16, (u64)nlink);
    kstat_put_u32(k, 24, mode);
    kstat_put_u32(k, 28, 0);                     /* uid */
    kstat_put_u32(k, 32, 0);                     /* gid */
    kstat_put_u64(k, 40, rdev);
    kstat_put_u64(k, 48, size);
    kstat_put_u64(k, 56, 4096);                  /* blksize */
    blocks = (size + 511) / 512;
    kstat_put_u64(k, 64, blocks);
}

static u32 copy_kstat_to_user(u64 ubuf, const char *k) {
    u64 as = sched_current_as_h();
    u32 i;
    if (as == 0 || ubuf == 0 || !mem.as_user_range_ok(as, ubuf, STAT_SIZE)) {
        return 0;
    }
    {
        volatile char *u = (volatile char *)ubuf;
        for (i = 0; i < STAT_SIZE; i++) {
            u[i] = k[i];
        }
    }
    return 1;
}

/* Populate kstat for a kernel open-file handle (ramfs/ext2/pipe/sock). */
static u32 kstat_from_fh(u32 fh, char *k) {
    u32 ino = 0;
    u32 mode = 0;
    u64 size = 0;
    u32 rdev = 0;
    u32 nlink = 1;
    if (fh == FD_BAD) {
        return 0;
    }
    if (pipe_fh_is(fh)) {
        kstat_fill(k, (u64)fh, S_IFIFO | 0x1B6, 0, 0, 1);
        return 1;
    }
    if (sock_fh_is(fh)) {
        kstat_fill(k, (u64)fh, S_IFSOCK | 0x1B6, 0, 0, 1);
        return 1;
    }
    if (fs.ext2_fh_is(fh)) {
        if (!fs.ext2_fstat(fh, &ino, &mode, &size, &nlink)) {
            return 0;
        }
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1);
        return 1;
    }
    ino = fs.fs_file_ino(fh);
    if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
        return 0;
    }
    kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink);
    return 1;
}

static u32 kstat_from_path_flags(const char *path, char *k, u32 follow) {
    u32 ino = 0;
    u32 mode = 0;
    u64 size = 0;
    u32 rdev = 0;
    u32 nlink = 1;
    if (path == 0) {
        return 0;
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()) {
            return 0;
        }
        if (follow) {
            if (!fs.ext2_stat_path(path, &ino, &mode, &size, &nlink)) {
                return 0;
            }
        } else {
            if (!fs.ext2_stat_path_nofollow(path, &ino, &mode, &size, &nlink)) {
                return 0;
            }
        }
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1);
        return 1;
    }
    ino = follow ? fs.fs_lookup(path) : fs.fs_lookup_nofollow(path);
    if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
        return 0;
    }
    kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink);
    return 1;
}

static u32 kstat_from_path(const char *path, char *k) {
    return kstat_from_path_flags(path, k, 1);
}

u64 sys_fstat(u64 fd, u64 ustat) {
    u32 fh = sched_fd_get((u32)fd);
    char k[STAT_SIZE];
    if (fh == FD_BAD) {
        return (u64)(-1);
    }
    if (!kstat_from_fh(fh, k)) {
        return (u64)(-1);
    }
    return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
}

u64 sys_stat(u64 upath, u64 ustat) {
    char path[PATH_KMAX];
    char k[STAT_SIZE];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (!kstat_from_path(path, k)) {
        return (u64)(-1);
    }
    return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
}

u64 sys_lstat(u64 upath, u64 ustat) {
    char path[PATH_KMAX];
    char k[STAT_SIZE];
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (!kstat_from_path_flags(path, k, 0)) {
        return (u64)(-1);
    }
    return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
}

/* newfstatat(dfd, path, statbuf, flags): AT_FDCWD + cwd-relative paths,
 * AT_EMPTY_PATH on a real fd, and relative names under a directory fd. */
u64 sys_newfstatat(u64 dfd, u64 upath, u64 ustat, u64 flags) {
    char rel[PATH_KMAX];
    char path[PATH_KMAX];
    char k[STAT_SIZE];
    u32 fh;
    u32 ino;
    u32 mode;
    u64 size;
    u32 rdev;
    u32 nlink;
    u32 follow = ((flags & (u64)AT_SYMLINK_NOFOLLOW) == 0) ? 1 : 0;

    if ((flags & (u64)AT_EMPTY_PATH) != 0) {
        /* May have empty path: fstat(dfd). */
        if (!copy_user_cstring(rel, upath, PATH_KMAX - 1)) {
            return (u64)(-1);
        }
        if (rel[0] == 0) {
            return sys_fstat(dfd, ustat);
        }
    }

    if ((u32)dfd == AT_FDCWD_U) {
        if (follow) {
            return sys_stat(upath, ustat);
        }
        return sys_lstat(upath, ustat);
    }

    if (!copy_user_cstring(rel, upath, PATH_KMAX - 1)) {
        return (u64)(-1);
    }
    if (rel[0] == '/') {
        /* Absolute path ignores dfd. */
        if (!canon_path("/", rel, path, PATH_KMAX)) {
            return (u64)(-1);
        }
        if (!kstat_from_path_flags(path, k, follow)) {
            return (u64)(-1);
        }
        return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
    }

    fh = sched_fd_get((u32)dfd);
    if (fh == FD_BAD || pipe_fh_is(fh) || sock_fh_is(fh)) {
        return (u64)(-1);
    }
    if (fs.ext2_fh_is(fh)) {
        if (!fs.ext2_stat_at_flags(fh, rel, &ino, &mode, &size, &nlink, follow)) {
            return (u64)(-1);
        }
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1);
        return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
    }
    if (!fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }
    ino = fs.fs_lookup_at_flags(fs.fs_file_ino(fh), rel, follow);
    if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
        return (u64)(-1);
    }
    kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink);
    return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
}

u64 sys_symlink(u64 utarget, u64 ulinkpath) {
    char target[PATH_KMAX];
    char linkpath[PATH_KMAX];
    if (!copy_user_cstring(target, utarget, PATH_KMAX - 1)) {
        return (u64)(-1);
    }
    if (!copy_user_path(linkpath, ulinkpath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(linkpath)) {
        return fs.ext2_mounted() && fs.ext2_symlink(target, linkpath) ? 0 : (u64)(-1);
    }
    return fs.fs_symlink(target, linkpath) != 0 ? 0 : (u64)(-1);
}

u64 sys_readlink(u64 upath, u64 ubuf, u64 bufsiz) {
    char path[PATH_KMAX];
    char kbuf[PATH_KMAX];
    u64 as = sched_current_as_h();
    u64 n;
    u64 i;
    u64 want;
    if (bufsiz == 0 || as == 0) {
        return (u64)(-1);
    }
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    want = bufsiz;
    if (want > PATH_KMAX) {
        want = PATH_KMAX;
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()) {
            return (u64)(-1);
        }
        n = fs.ext2_readlink(path, kbuf, want);
    } else {
        n = fs.fs_readlink(path, kbuf, want);
    }
    if (n == (u64)(-1)) {
        return (u64)(-1);
    }
    if (!mem.as_user_range_ok(as, ubuf, n)) {
        return (u64)(-1);
    }
    {
        volatile char *u = (volatile char *)ubuf;
        for (i = 0; i < n; i++) {
            u[i] = kbuf[i];
        }
    }
    return n;
}

u64 sys_sync(void) {
    if (fs.ext2_mounted()) {
        fs.ext2_sync();
    }
    return 0;
}

/* fsync(fd): flush Ext2 volume if fd is an Ext2 handle; else no-op success.
 * Not a per-inode journal commit — pairs with journal-lite ordered markers. */
u64 sys_fsync(u64 fd) {
    u32 fh;
    if (fd >= 16) return (u64)(-1);
    fh = sched_fd_get((u32)fd);
    if (fh == FD_BAD || fh == 0xFFFFFFFF) return (u64)(-1);
    if (fs.ext2_mounted() && fs.ext2_fh_is(fh)) {
        fs.ext2_sync();
    }
    return 0;
}



/* Shared readiness probe used by select(2) and poll(2). */
static u32 fd_ready_read(u32 fh) {
    if (sock_fh_is(fh)) {
        return sock_can_read(fh);
    }
    if (pipe_fh_is(fh)) {
        return pipe_can_read(fh);
    }
    if (!fs.ext2_fh_is(fh) && fs.fs_file_is_console(fh)) {
        return tty_input_pending();
    }
    return 1; /* regular files readable */
}

static u32 fd_ready_write(u32 fh) {
    if (sock_fh_is(fh)) {
        return sock_can_write(fh);
    }
    if (pipe_fh_is(fh)) {
        return pipe_can_write(fh);
    }
    if (!fs.ext2_fh_is(fh) && fs.fs_file_is_console(fh)) {
        return 1;
    }
    return 1; /* regular files */
}

enum {
    EINTR_SEL = (u64)-4,
    POLLIN_F = 0x0001,
    POLLOUT_F = 0x0004,
    POLLERR_F = 0x0008,
    POLLNVAL_F = 0x0020
};

/* select(2): nfds<=64, one-word fd_set bitmasks, optional timeval.
 * Interruptible: pending fatal / non-SA_RESTART handler -> EINTR. */
u64 sys_select(u64 nfds, u64 readfds_u, u64 writefds_u, u64 exceptfds_u,
               u64 timeout_u) {
    u64 as = sched_current_as_h();
    u64 rbits = 0;
    u64 wbits = 0;
    u64 ebits = 0;
    u64 out_r = 0;
    u64 out_w = 0;
    u64 deadline = 0;
    u32 block = 0;
    u32 i;
    u32 nready;

    if (as == 0 || nfds > 64) {
        return (u64)(-1);
    }
    if (exceptfds_u != 0) {
        /* exceptfds ignored; clear on return if provided */
        if (!mem.as_user_range_ok(as, exceptfds_u, 8)) {
            return (u64)(-1);
        }
        ebits = *(volatile u64 *)exceptfds_u;
    }
    if (readfds_u != 0) {
        if (!mem.as_user_range_ok(as, readfds_u, 8)) {
            return (u64)(-1);
        }
        rbits = *(volatile u64 *)readfds_u;
    }
    if (writefds_u != 0) {
        if (!mem.as_user_range_ok(as, writefds_u, 8)) {
            return (u64)(-1);
        }
        wbits = *(volatile u64 *)writefds_u;
    }
    if (timeout_u == 0) {
        block = 1; /* infinite */
    } else {
        i64 sec;
        i64 usec;
        u64 ms;
        if (!mem.as_user_range_ok(as, timeout_u, 16)) {
            return (u64)(-1);
        }
        sec = (i64)(*(volatile u64 *)timeout_u);
        usec = (i64)(*(volatile u64 *)(timeout_u + 8));
        if (sec < 0 || usec < 0) {
            return (u64)(-1);
        }
        if (sec == 0 && usec == 0) {
            block = 0;
            deadline = sched_tick_count();
        } else {
            block = 1;
            /* 100 Hz ticks */
            ms = (u64)sec * 100u + (u64)usec / 10000u;
            if (ms == 0) {
                ms = 1;
            }
            deadline = sched_tick_count() + ms;
        }
    }
    (void)ebits;

    for (;;) {
        out_r = 0;
        out_w = 0;
        nready = 0;
        for (i = 0; i < (u32)nfds; i++) {
            u64 bit = (u64)1 << i;
            u32 fh;
            u32 want_r = (rbits & bit) != 0;
            u32 want_w = (wbits & bit) != 0;
            u32 this_ready = 0;
            if (!want_r && !want_w) {
                continue;
            }
            fh = sched_fd_get(i);
            if (fh == FD_BAD) {
                continue;
            }
            if (want_r && fd_ready_read(fh)) {
                out_r |= bit;
                this_ready = 1;
            }
            if (want_w && fd_ready_write(fh)) {
                out_w |= bit;
                this_ready = 1;
            }
            if (this_ready) {
                nready++;
            }
        }
        if (nready > 0) {
            break;
        }
        if (!block) {
            break;
        }
        if (timeout_u != 0 && sched_tick_count() >= deadline) {
            break;
        }
        sched_check_signals();
        if (sched_should_eintr()) {
            return EINTR_SEL;
        }
        /* Infinite block: channel-sleep. Finite timeout: yield so deadline
         * can be observed without a timer->select ISR coupling. */
        if (timeout_u == 0) {
            sched_sleep((u64)&select_wait_token);
        } else {
            sched_yield();
        }
    }

    if (readfds_u != 0) {
        *(volatile u64 *)readfds_u = out_r;
    }
    if (writefds_u != 0) {
        *(volatile u64 *)writefds_u = out_w;
    }
    if (exceptfds_u != 0) {
        *(volatile u64 *)exceptfds_u = 0;
    }
    return (u64)nready;
}

/* poll(2): nfds<=64, struct pollfd {int fd; short events; short revents}.
 * timeout_ms < 0 blocks; 0 is non-blocking; >0 is millisecond budget (100 Hz). */
u64 sys_poll(u64 ufds, u64 nfds, u64 timeout_ms) {
    u64 as = sched_current_as_h();
    u64 deadline = 0;
    u32 block = 0;
    u32 i;
    u32 nready;

    if (as == 0 || nfds > 64) {
        return (u64)(-1);
    }
    if (nfds > 0) {
        if (!mem.as_user_range_ok(as, ufds, nfds * 8)) {
            return (u64)(-1);
        }
    }
    if ((i64)timeout_ms < 0) {
        block = 1;
    } else if (timeout_ms == 0) {
        block = 0;
    } else {
        u64 ticks = timeout_ms / 10u;
        if (ticks == 0) {
            ticks = 1;
        }
        block = 1;
        deadline = sched_tick_count() + ticks;
    }

    for (;;) {
        nready = 0;
        for (i = 0; i < (u32)nfds; i++) {
            volatile u32 *slot = (volatile u32 *)(ufds + (u64)i * 8);
            u32 fd = slot[0];
            u16 events = (u16)(slot[1] & 0xFFFF);
            u16 revents = 0;
            u32 fh;
            slot[1] = (u32)events; /* clear revents (high 16) */
            if ((s32)fd < 0) {
                continue;
            }
            if (fd >= 16) {
                revents = POLLNVAL_F;
            } else {
                fh = sched_fd_get(fd);
                if (fh == FD_BAD) {
                    revents = POLLNVAL_F;
                } else {
                    if ((events & POLLIN_F) != 0 && fd_ready_read(fh)) {
                        revents = (u16)(revents | POLLIN_F);
                    }
                    if ((events & POLLOUT_F) != 0 && fd_ready_write(fh)) {
                        revents = (u16)(revents | POLLOUT_F);
                    }
                }
            }
            slot[1] = ((u32)revents << 16) | (u32)events;
            if (revents != 0) {
                nready++;
            }
        }
        if (nready > 0) {
            return (u64)nready;
        }
        if (!block) {
            return 0;
        }
        if ((i64)timeout_ms >= 0 && sched_tick_count() >= deadline) {
            return 0;
        }
        sched_check_signals();
        if (sched_should_eintr()) {
            return EINTR_SEL;
        }
        if ((i64)timeout_ms < 0) {
            sched_sleep((u64)&select_wait_token);
        } else {
            sched_yield();
        }
    }
}
