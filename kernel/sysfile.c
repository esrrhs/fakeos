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
    PATH_KMAX  = 512,
    CWD_LEN    = 512,
    IO_CHUNK   = 4096,
    DENT_CHUNK = 1920,             /* bounded variable-dirent staging buffer */
    FD_BAD     = 0xFFFFFFFF,

    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,

    INO_DIR_K = 2,                 /* fs INO_DIR numeric contract */

    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,

    SYMLOOP_MAX_CWD = 8          /* match fs SYMLOOP_MAX for physical cwd */
};

/* sched.c (same package) */
extern u64 sched_current_as_h(void);
extern u32 sched_fd_get(u32 idx);
extern void sched_fd_set(u32 idx, u32 fh);
extern u32 sched_fd_alloc(void);
extern u32 sched_fd_close_slot(u32 idx);
extern void sched_fd_dup2_install(u32 newfd, u32 ofh);
extern const char *sched_cwd(void);
extern void sched_cwd_set(const char *kpath);

/* kernel/tty.c (canonical serial input) */
extern u64 tty_console_read(u64 ubuf, u64 len);
extern u64 tty_jobctl_write_wait(void);
extern u32 tty_input_pending(void);
extern u64 sched_clock_ticks(void);
extern void sched_yield(void);
extern void sched_sleep(u64 chan);
extern void sched_park_prepare(u64 chan);
extern void sched_timer_arm_ticks(u64 abs_ticks);
extern u32 sched_park_commit(u64 chan, u32 nonrestartable);
enum { PARK_INTERRUPTED_F = 2 }; /* matches sched.c park commit result */
extern void sched_wakeup(u64 chan);
extern u32 pipe_can_read(u32 fh);
extern u32 pipe_can_write(u32 fh);

/* Wait channel for select/poll; woken by tty/pipe/sock readiness. */
static u32 select_wait_token;

void select_kick(void) {
    sched_wakeup((u64)&select_wait_token);
}
extern u32 sched_should_eintr_timer(void);
extern void sched_check_signals(void);
extern u32 sock_can_read(u32 fh);
extern u32 sock_can_write(u32 fh);

/* kernel/epoll.c */
extern u32 eventfd_fh_is(u32 fh);
extern u32 epoll_fh_is(u32 fh);
extern u32 eventfd_can_read(u32 fh);
extern u32 eventfd_can_write(u32 fh);
extern u32 epoll_can_read(u32 fh);
extern u32 epoll_can_write(u32 fh);
extern u64 eventfd_read(u32 fh, char *kbuf, u64 len);
extern u64 eventfd_write(u32 fh, const char *kbuf, u64 len);
extern u32 eventfd_retain(u32 fh);
extern u32 epoll_retain(u32 fh);
extern u32 eventfd_close(u32 fh);
extern u32 epoll_close(u32 fh);

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
    /* maxlen counts non-NUL bytes; dst has maxlen+1 capacity. */
    for (;;) {
        char c = p[i];
        if (c == 0) {
            dst[i] = 0;
            return 1;
        }
        if (i == maxlen) return 0; /* no terminator within the caller's cap */
        dst[i] = c;
        i++;
        if (((uva + i) & 0xFFF) == 0) {
            if (!mem.as_user_range_ok(as, (uva + i) & ~0xFFFULL, 4096)) {
                return 0;
            }
        }
    }
}


/* Kernel readlink for an absolute path (Ramfs or Ext2). */
static u64 k_readlink(const char *path, char *kbuf, u64 bufsiz) {
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()) {
            return (u64)(-1);
        }
        return fs.ext2_readlink(path, kbuf, bufsiz);
    }
    return fs.fs_readlink(path, kbuf, bufsiz);
}

/* Namespace paths must be walked before selecting the filesystem. Backends
 * resolve links within their own root, so dispatching /tmp/link -> /mnt/file
 * (or the reverse) before following it cannot work. Keep the final component
 * intact for entry operations such as unlink/lstat/O_CREAT|O_EXCL. */
static u32 path_join_raw(const char *base, const char *part, char *out, u32 cap) {
    u32 n = 0;
    u32 i = 0;
    if (part[0] == 0) return 0;
    if (part[0] != '/') {
        while (base[i] != 0) {
            if (n + 1 >= cap) return 0;
            out[n++] = base[i++];
        }
        if (n == 0 || out[n - 1] != '/') {
            if (n + 1 >= cap) return 0;
            out[n++] = '/';
        }
    }
    i = 0;
    while (part[i] != 0) {
        if (n + 1 >= cap) return 0;
        out[n++] = part[i++];
    }
    out[n] = 0;
    return 1;
}

/* 0 missing, 1 directory, 2 existing non-directory. */
static u32 path_dir_state(const char *path) {
    u32 ino = 0;
    u32 mode = 0;
    u64 size = 0;
    u32 nlink = 0;
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted() ||
            !fs.ext2_stat_path_nofollow(path, &ino, &mode, &size, &nlink)) return 0;
        return (mode & 0xF000) == 0x4000 ? 1 : 2;
    }
    ino = fs.fs_lookup_nofollow(path);
    if (!ino) return 0;
    return fs.fs_ino_kind(ino) == INO_DIR_K ? 1 : 2;
}

static u32 path_physical(const char *in, char *out, u32 cap, u32 follow) {
    char work[PATH_KMAX];
    char prefix[PATH_KMAX];
    char tgt[PATH_KMAX];
    char rebuilt[PATH_KMAX];
    u32 loop;
    u32 i;
    if (in == 0 || out == 0 || cap < 2 || in[0] != '/') return 0;
    if (kstr_len(in) + 1 > PATH_KMAX) return 0;
    for (i = 0; ; i++) {
        work[i] = in[i];
        if (in[i] == 0) break;
    }
    for (loop = 0; loop <= SYMLOOP_MAX_CWD; loop++) {
        u32 found = 0;
        u32 wi = 0;
        prefix[0] = '/';
        prefix[1] = 0;
        while (work[wi] != 0) {
            u32 start;
            u32 n = 0;
            u32 rest;
            u64 rn;
            while (work[wi] == '/') wi++;
            if (work[wi] == 0) break;
            start = wi;
            while (work[wi + n] != 0 && work[wi + n] != '/') n++;
            wi += n;
            if (n == 1 && work[start] == '.') continue;
            if (n == 2 && work[start] == '.' && work[start + 1] == '.') {
                canon_pop(prefix);
                continue;
            }
            if (!canon_push(prefix, &work[start], n, PATH_KMAX)) return 0;
            rest = wi;
            while (work[rest] == '/') rest++;
            /* Modes: 0 entry mutation, 1 follow, 2 lstat/readlink (a trailing
             * slash still dereferences). Never collapse .. before links. */
            rn = ((!follow && work[rest] == 0) ||
                  (follow == 2 && work[wi] == 0)) ? (u64)(-1) :
                 k_readlink(prefix, tgt, PATH_KMAX - 1);
            if (rn != (u64)(-1)) {
                u32 k;
                if (loop == SYMLOOP_MAX_CWD || rn == 0 || rn >= PATH_KMAX) return 0;
                tgt[(u32)rn] = 0;
                canon_pop(prefix);
                if (!path_join_raw(prefix, tgt, rebuilt, PATH_KMAX)) return 0;
                /* Append the unprocessed suffix verbatim, including .. . */
                n = kstr_len(rebuilt);
                for (k = wi; work[k] != 0; k++) {
                    if (n + 1 >= PATH_KMAX) return 0;
                    rebuilt[n++] = work[k];
                }
                rebuilt[n] = 0;
                for (k = 0; k <= n; k++) work[k] = rebuilt[k];
                found = 1;
                break;
            }
            if (work[wi] != 0) {
                u32 state = path_dir_state(prefix);
                /* Only entry creation may leave a missing final component
                 * before trailing slashes, e.g. mkdir("new/"). A symlink/
                 * file is never a directory entry suitable for rmdir. */
                if (state != 1 && (work[rest] != 0 || follow || state != 0)) return 0;
            }
        }
        if (!found) {
            if (kstr_len(prefix) + 1 > cap) return 0;
            for (i = 0; ; i++) {
                out[i] = prefix[i];
                if (prefix[i] == 0) break;
            }
            return 1;
        }
    }
    return 0;
}

u32 copy_user_path_follow(char *dst, u64 upath, u32 follow) {
    char raw[PATH_KMAX];
    char absolute[PATH_KMAX];
    if (!copy_user_cstring(raw, upath, PATH_KMAX - 1) ||
        !path_join_raw(sched_cwd(), raw, absolute, PATH_KMAX)) return 0;
    return path_physical(absolute, dst, PATH_KMAX, follow);
}

/* Entry operations follow parents, but not the final directory entry. */
u32 copy_user_path(char *dst, u64 upath) {
    return copy_user_path_follow(dst, upath, 0);
}

u64 sys_open(u64 upath, u64 flags, u64 mode) {
    char path[PATH_KMAX];
    u32 fh;
    u32 fd;
    u32 cmode = (u32)mode;
    if (!copy_user_path_follow(path, upath,
            (flags & 0xC0) == 0xC0 ? 0 : ((flags & 0x20000) ? 2 : 1))) {
        return (u64)(-1);
    }
    if (flags & 0x20000) {
        char target[PATH_KMAX];
        if (k_readlink(path, target, PATH_KMAX - 1) != (u64)(-1)) return (u64)(-40);
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()) {
            return (u64)(-1);
        }
        fh = fs.ext2_open_mode(path, (u32)flags, cmode);
    } else {
        fh = fs.fs_file_open_mode(path, (u32)flags, cmode);
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
    return sched_fd_close_slot((u32)fd) ? 0 : (u64)(-1);
}

/* dup2(oldfd, newfd): make newfd refer to the same open-file as oldfd. */
u64 sys_dup2(u64 oldfd, u64 newfd) {
    u32 ofh;
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
    if (fs.ext2_fh_is(ofh)) {
        fs.ext2_file_retain(ofh);
    } else if (pipe_fh_is(ofh)) {
        pipe_retain(ofh);
    } else if (sock_fh_is(ofh)) {
        sock_retain(ofh);
    } else if (eventfd_fh_is(ofh)) {
        eventfd_retain(ofh);
    } else if (epoll_fh_is(ofh)) {
        epoll_retain(ofh);
    } else {
        fs.fs_file_retain(ofh);
    }
    sched_fd_dup2_install((u32)newfd, ofh);
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

    if (eventfd_fh_is(fh)) {
        u64 n;
        if (!mem.as_user_range_ok(as, buf, 8) || len < 8) {
            return (u64)(-22);
        }
        n = eventfd_read(fh, kbuf, 8);
        if (n == 8) {
            /* 8-byte little-endian counter to user memory. */
            volatile char *u = (volatile char *)buf;
            u64 i;
            for (i = 0; i < 8; i++) {
                u[i] = kbuf[i];
            }
        }
        return n;
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
        /* Check before consuming file data: an invalid destination must
         * not silently advance the file offset by a discarded chunk. */
        if (!mem.as_user_range_ok(as, buf + done, want)) {
            return done != 0 ? done : (u64)(-1);
        }
        n = fs.ext2_fh_is(fh) ? fs.ext2_read(fh, kbuf, want) : fs.fs_read_h(fh, kbuf, want);
        if (n == (u64)(-1)) {
            return done != 0 ? done : (u64)(-1);
        }
        if (n == 0) {
            break;                   /* EOF */
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

    if (eventfd_fh_is(fh)) {
        u64 n;
        if (!mem.as_user_range_ok(as, buf, 8) || len < 8) {
            return (u64)(-22);
        }
        {
            volatile char *u = (volatile char *)buf;
            u64 i;
            for (i = 0; i < 8; i++) {
                kbuf[i] = u[i];
            }
        }
        n = eventfd_write(fh, kbuf, 8);
        return n;
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
    if (!copy_user_path_follow(path, upath, 1)) {
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
    if (kstr_len(path) >= CWD_LEN) return (u64)(-1);
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

/* getdents64: stream complete variable-length records from a directory descriptor.
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
    /* Validate before enumeration advances its cookie. Faulting destinations
     * must not silently consume entries; include overflow and empty buffers. */
    if (len < 48 || ubuf + len < ubuf || !mem.as_user_range_ok(as, ubuf, len)) {
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

static void kstat_fill(char *k, u64 ino, u32 mode, u64 size, u64 rdev, u32 nlink,
                       u32 uid, u32 gid, u32 mtime) {
    u64 blocks;
    kstat_zero(k);
    kstat_put_u64(k, 0, 1);                      /* Ramfs device identity */
    kstat_put_u64(k, 8, ino);
    kstat_put_u64(k, 16, (u64)nlink);
    kstat_put_u32(k, 24, mode);
    kstat_put_u32(k, 28, uid);
    kstat_put_u32(k, 32, gid);
    kstat_put_u64(k, 40, rdev);
    kstat_put_u64(k, 48, size);
    kstat_put_u64(k, 56, 4096);                  /* blksize */
    blocks = (size + 511) / 512;
    kstat_put_u64(k, 64, blocks);
    /* timespecs: atime/mtime/ctime share mtime (boot-relative seconds). */
    kstat_put_u64(k, 72, (u64)mtime);
    kstat_put_u64(k, 88, (u64)mtime);
    kstat_put_u64(k, 104, (u64)mtime);
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
    u32 uid = 0;
    u32 gid = 0;
    u32 mtime = 0;
    if (fh == FD_BAD) {
        return 0;
    }
    if (pipe_fh_is(fh)) {
        kstat_fill(k, (u64)fh, S_IFIFO | 0x1B6, 0, 0, 1, 0, 0, 0);
        return 1;
    }
    if (sock_fh_is(fh)) {
        kstat_fill(k, (u64)fh, S_IFSOCK | 0x1B6, 0, 0, 1, 0, 0, 0);
        return 1;
    }
    if (fs.ext2_fh_is(fh)) {
        if (!fs.ext2_fstat(fh, &ino, &mode, &size, &nlink)) {
            return 0;
        }
        fs.ext2_owner_mtime(ino, &uid, &gid, &mtime);
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1, uid, gid, mtime);
        kstat_put_u64(k, 0, 2); /* Ext2 is distinct from Ramfs inode numbers. */
        return 1;
    }
    ino = fs.fs_file_ino(fh);
    if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
        return 0;
    }
    fs.fs_owner_mtime(ino, &uid, &gid, &mtime);
    kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink, uid, gid, mtime);
    return 1;
}

static u32 kstat_from_path_flags(const char *path, char *k, u32 follow) {
    char resolved[PATH_KMAX];
    u32 ino = 0;
    u32 mode = 0;
    u64 size = 0;
    u32 rdev = 0;
    u32 nlink = 1;
    u32 uid = 0;
    u32 gid = 0;
    u32 mtime = 0;
    if (path == 0 || !path_physical(path, resolved, PATH_KMAX, follow ? 1 : 2)) {
        return 0;
    }
    path = resolved;
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
        fs.ext2_owner_mtime(ino, &uid, &gid, &mtime);
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1, uid, gid, mtime);
        kstat_put_u64(k, 0, 2); /* Ext2 is distinct from Ramfs inode numbers. */
        return 1;
    }
    ino = follow ? fs.fs_lookup(path) : fs.fs_lookup_nofollow(path);
    if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
        return 0;
    }
    fs.fs_owner_mtime(ino, &uid, &gid, &mtime);
    kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink, uid, gid, mtime);
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
    if (!copy_user_path_follow(path, upath, 1)) {
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
    if (!copy_user_path_follow(path, upath, 2)) {
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
        if (!path_join_raw("/", rel, path, PATH_KMAX)) {
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
        u32 uid = 0;
        u32 gid = 0;
        u32 mtime = 0;
        if (!fs.ext2_stat_at_flags(fh, rel, &ino, &mode, &size, &nlink, follow)) {
            return (u64)(-1);
        }
        fs.ext2_owner_mtime(ino, &uid, &gid, &mtime);
        kstat_fill(k, (u64)ino, mode, size, 0, nlink ? nlink : 1, uid, gid, mtime);
        kstat_put_u64(k, 0, 2); /* Ext2 is distinct from Ramfs inode numbers. */
        return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
    }
    if (!fs.fs_file_is_dir(fh)) {
        return (u64)(-1);
    }
    {
        u32 uid = 0;
        u32 gid = 0;
        u32 mtime = 0;
        ino = fs.fs_lookup_at_flags(fs.fs_file_ino(fh), rel, follow);
        if (ino == 0 || !fs.fs_stat_ino(ino, &mode, &size, &rdev, &nlink)) {
            return (u64)(-1);
        }
        fs.fs_owner_mtime(ino, &uid, &gid, &mtime);
        kstat_fill(k, (u64)ino, mode, size, (u64)rdev, nlink, uid, gid, mtime);
        return copy_kstat_to_user(ustat, k) ? 0 : (u64)(-1);
    }
}

u64 sys_link(u64 uold, u64 unew) {
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
        return (u64)(-1);              /* EXDEV */
    }
    if (o_ext2) {
        return fs.ext2_mounted() && fs.ext2_link(oldp, newp) ? 0 : (u64)(-1);
    }
    return fs.fs_link(oldp, newp) ? 0 : (u64)(-1);
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
    if (!copy_user_path_follow(path, upath, 2)) {
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



/* Shared readiness probe used by select(2)/poll(2)/epoll(2). NOTE: these
 * take the target object's own (possibly sleeping) locks — never call them
 * while holding a spinlock. */
u32 fd_ready_read(u32 fh) {
    if (eventfd_fh_is(fh)) {
        return eventfd_can_read(fh);
    }
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

u32 fd_ready_write(u32 fh) {
    if (eventfd_fh_is(fh)) {
        return eventfd_can_write(fh);
    }
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
        if (sec < 0 || usec < 0 || usec >= 1000000) {
            return (u64)(-1);
        }
        if (sec == 0 && usec == 0) {
            block = 0;
            deadline = sched_clock_ticks();
        } else {
            block = 1;
            /* 100 Hz ticks */
            if ((u64)sec > (~0ULL - 101) / 100u) return (u64)(-1);
            ms = (u64)sec * 100u + ((u64)usec + 9999u) / 10000u + 1;
            deadline = sched_clock_ticks();
            if (ms > ~0ULL - deadline) return (u64)(-1);
            deadline += ms;
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
        if (timeout_u != 0 && sched_clock_ticks() >= deadline) {
            break;
        }
        sched_check_signals();
        if (sched_should_eintr_timer()) {
            return EINTR_SEL;
        }
        /* Infinite block: channel-sleep. Finite timeout: yield so deadline
         * can be observed without a timer->select ISR coupling. Readiness is
         * a predicate checked outside a condition lock, so the park intent is
         * registered before sleeping: a readiness producer latches the
         * notification (PARK_WOKEN) instead of the wakeup being lost, and
         * commit refuses to park when cancellation is pending (nonrestartable
         * policy). */
        sched_park_prepare((u64)&select_wait_token);
        if (timeout_u != 0) {
            sched_timer_arm_ticks(deadline);
        }
        if (sched_park_commit((u64)&select_wait_token, 1)
            == PARK_INTERRUPTED_F) {
            return EINTR_SEL;
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
        u64 ticks = timeout_ms / 10u + (timeout_ms % 10u != 0) + 1;
        block = 1;
        deadline = sched_clock_ticks();
        if (ticks > ~0ULL - deadline) return (u64)(-1);
        deadline += ticks;
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
        if ((i64)timeout_ms >= 0 && sched_clock_ticks() >= deadline) {
            return 0;
        }
        sched_check_signals();
        if (sched_should_eintr_timer()) {
            return EINTR_SEL;
        }
        sched_park_prepare((u64)&select_wait_token);
        if ((i64)timeout_ms >= 0) {
            sched_timer_arm_ticks(deadline);
        }
        if (sched_park_commit((u64)&select_wait_token, 1)
            == PARK_INTERRUPTED_F) {
            return EINTR_SEL;
        }
    }
}

/* utimensat(2): set mtime (atime ignored — ramfs/ext2 only track mtime).
 * Thin: AT_FDCWD (+ absolute paths); times=NULL => now; UTIME_OMIT/UTIME_NOW
 * on times[1].tv_nsec. AT_SYMLINK_NOFOLLOW honored. */
u64 sys_utimensat(u64 dfd, u64 upath, u64 utimes, u64 flags) {
    char path[PATH_KMAX];
    u64 as = sched_current_as_h();
    u32 follow;
    u32 mtime_sec;
    u32 omit;
    i64 sec;
    i64 nsec;
    enum {
        AT_FDCWD_U = 0xFFFFFF9C,
        AT_SYMLINK_NOFOLLOW = 0x100,
        UTIME_NOW = 0x3fffffff,
        UTIME_OMIT = 0x3ffffffe,
        EINVAL_NEG = (u64)-22
    };

    if (as == 0 || upath == 0) {
        return (u64)(-1);
    }
    if ((flags & ~(u64)AT_SYMLINK_NOFOLLOW) != 0) {
        return EINVAL_NEG;
    }
    follow = ((flags & (u64)AT_SYMLINK_NOFOLLOW) == 0) ? 1 : 0;

    if ((u32)dfd != AT_FDCWD_U) {
        /* Absolute path ignores dfd; relative under dirfd not yet. */
        char raw[PATH_KMAX];
        if (!copy_user_cstring(raw, upath, PATH_KMAX - 1)) {
            return (u64)(-1);
        }
        if (raw[0] != '/') {
            return (u64)(-1);
        }
        if (!path_join_raw("/", raw, path, PATH_KMAX)) {
            return (u64)(-1);
        }
    } else if (!copy_user_path_follow(path, upath, follow ? 1 : 2)) {
        return (u64)(-1);
    }

    {
        char resolved[PATH_KMAX];
        u32 i;
        if (!path_physical(path, resolved, PATH_KMAX, follow ? 1 : 2)) return (u64)(-1);
        for (i = 0; ; i++) {
            path[i] = resolved[i];
            if (resolved[i] == 0) break;
        }
    }
    omit = 0;
    if (utimes == 0) {
        mtime_sec = (u32)(sched_clock_ticks() / 100);
    } else {
        if (!mem.as_user_range_ok(as, utimes, 32)) {
            return (u64)(-1);
        }
        /* times[1] = mtime at offset 16. */
        sec = (i64)(*(volatile u64 *)(utimes + 16));
        nsec = (i64)(*(volatile u64 *)(utimes + 24));
        if (nsec == (i64)UTIME_OMIT) {
            omit = 1;
            mtime_sec = 0;
        } else if (nsec == (i64)UTIME_NOW) {
            mtime_sec = (u32)(sched_clock_ticks() / 100);
        } else if (nsec < 0 || nsec >= 1000000000) {
            return EINVAL_NEG;
        } else if (sec < 0) {
            return EINVAL_NEG;
        } else {
            mtime_sec = (u32)sec;
        }
    }
    if (omit) {
        return 0;
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted() || !fs.ext2_utimens(path, mtime_sec, follow)) {
            return (u64)(-1);
        }
        return 0;
    }
    return fs.fs_utimens(path, mtime_sec, follow) ? 0 : (u64)(-1);
}

/* Fill a Linux x86_64 struct statfs (120 bytes) at ustat. */
static u32 fill_statfs_user(u64 ustat, u64 bsize, u64 blocks, u64 bfree,
                            u64 files, u64 ffree) {
    u64 as = sched_current_as_h();
    volatile u64 *u;
    u32 i;
    if (as == 0 || ustat == 0 || !mem.as_user_range_ok(as, ustat, 120)) {
        return 0;
    }
    u = (volatile u64 *)ustat;
    for (i = 0; i < 15; i++) {
        u[i] = 0;
    }
    u[0] = 0x858458f6; /* RAMFS_MAGIC-ish / generic */
    u[1] = bsize;      /* f_bsize */
    u[2] = blocks;     /* f_blocks */
    u[3] = bfree;      /* f_bfree */
    u[4] = bfree;      /* f_bavail */
    u[5] = files;      /* f_files */
    u[6] = ffree;      /* f_ffree */
    /* f_fsid left 0 */
    u[8] = 255;        /* f_namelen */
    u[9] = bsize;      /* f_frsize */
    return 1;
}

/* statfs(2): Ramfs reports PMM pages as blocks; Ext2 uses on-disk counts. */
u64 sys_statfs(u64 upath, u64 ustat) {
    char path[PATH_KMAX];
    u64 bsize = 0;
    u64 blocks = 0;
    u64 bfree = 0;
    u64 files = 0;
    u64 ffree = 0;
    if (!copy_user_path_follow(path, upath, 1)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted()
            || !fs.ext2_statfs_info(&bsize, &blocks, &bfree, &files, &ffree)) {
            return (u64)(-1);
        }
        if (!fill_statfs_user(ustat, bsize, blocks, bfree, files, ffree)) {
            return (u64)(-1);
        }
        /* Tag Ext2 magic. */
        {
            u64 as = sched_current_as_h();
            if (as != 0 && mem.as_user_range_ok(as, ustat, 8)) {
                *(volatile u64 *)ustat = 0xEF53;
            }
        }
        return 0;
    }
    if (!fs.fs_statfs_info(&bsize, &blocks, &bfree, &files, &ffree)) {
        return (u64)(-1);
    }
    return fill_statfs_user(ustat, bsize, blocks, bfree, files, ffree)
           ? 0 : (u64)(-1);
}
