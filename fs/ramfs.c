package fs;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* kprintf lives in the kernel package; declaring it extern (instead of
 * importing kernel) keeps the dependency graph acyclic: kernel -> fs -> mem,
 * mirroring how mem/as.c already calls kprintf. */
extern void kprintf(const char *fmt, ...);

/* Filled by kernel for synthetic /proc/self nodes (see procfs_gen_*). */
extern u64 procfs_gen_status(char *buf, u64 cap);
extern u64 procfs_gen_maps(char *buf, u64 cap);
extern u64 procfs_gen_cmdline(char *buf, u64 cap);
extern u64 procfs_gen_environ(char *buf, u64 cap);
extern u32 sched_umask_get(void);

/* ---------------------------------------------------------------------------
 * Minimal in-memory VFS + Ramfs
 *
 * Objects:
 *   inode  - file, directory, symlink or the console character device
 *   dentry - a named {name -> inode} link held by its parent directory
 *   open file - one per open(2), carrying offset/refcount/flags
 *
 * Files come in two flavors:
 *   static (ro=1) - data points at a kernel rodata incbin image; immutable
 *   dynamic       - kmalloc-backed growable buffer (doubling, 1 MiB cap)
 *
 * All handles are plain u32 indices: inode numbers and open-file slots. This
 * matches the handle style used by mem/as.c across package boundaries.
 * ------------------------------------------------------------------------- */

enum {
    INO_FREE    = 0,
    INO_FILE    = 1,
    INO_DIR     = 2,
    INO_CONSOLE = 3,
    INO_NULL    = 4,               /* /dev/null: reads EOF, writes discard */
    INO_ZERO    = 5,               /* /dev/zero: reads zeros, writes discard */
    INO_PROC_STATUS  = 6,          /* /proc/self/status (snapshot on open) */
    INO_PROC_MAPS    = 7,          /* /proc/self/maps */
    INO_PROC_CMDLINE = 8,          /* /proc/self/cmdline */
    INO_SYMLINK = 9,               /* symbolic link; target in buf/size */
    INO_PROC_ENVIRON = 10,         /* /proc/self/environ (live per-process) */

    ROOT_INO    = 1,
    PROC_ENVIRON_INO = 57,
    PROC_STATUS_INO  = 58,
    PROC_MAPS_INO    = 59,
    PROC_CMDLINE_INO = 60,
    ZERO_INO    = 61,              /* fixed /dev/zero inode */
    NULL_INO    = 62,              /* fixed /dev/null inode */
    CONSOLE_INO = 63,
    MAX_INODES  = 256,
    MAX_DENTS   = 64,
    NAME_LEN    = 32,               /* 31 usable chars + NUL */
    NAME_MAX    = 31,

    MAX_OPEN_FILES = 64,
    CONSOLE_FH     = 1,            /* permanent slot, fd 0/1/2 point at it */
    FH_BAD         = 0,

    DENT_RECLEN = 48,             /* fixed-size getdents64 record */

    /* Dynamic files are kmalloc-backed; cap bounds worst-case buddy
     * pressure (tool outputs are a few hundred KB, margin for growth). */
    FILE_MAX = 16777216,          /* 16 MiB per-file growth ceiling */

    O_RDONLY  = 0,
    O_WRONLY  = 1,
    O_RDWR    = 2,
    O_CREAT   = 0x040,
    O_EXCL    = 0x080,
    O_TRUNC   = 0x200,
    O_APPEND  = 0x400,

    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,

    DT_REG = 8,
    DT_DIR = 4,
    DT_CHR = 2,
    DT_LNK = 10,

    SYMLOOP_MAX = 8,
    SYMLINK_MAX = 127              /* target bytes (not counting NUL) */
};

struct dent {
    char name[NAME_LEN];
    u32 ino;
};

struct inode {
    u32 type;
    u32 parent;
    u32 ndent;
    u32 ro;                        /* static incbin-backed file */
    u32 mode;                      /* Linux S_IF* | permission bits */
    struct dent dents[MAX_DENTS];
    const char *sdata;             /* static data (kernel rodata) */
    char *buf;                     /* dynamic data (kmalloc) */
    u64 size;
    u64 cap;
};

struct open_file {
    u32 used;
    u32 ino;
    u64 off;
    u32 refs;
    u32 readable;
    u32 writable;
    u32 append;                    /* O_APPEND: reposition to EOF on write */
    u32 permanent;                 /* never freed (console) */
    char *snap;                    /* /proc/self snapshot (kmalloc); shared via refs */
    u64 snap_len;
};

static struct inode inodes[MAX_INODES];
static struct open_file opens[MAX_OPEN_FILES];

static u32 inode_is_proc(u32 type) {
    return type == INO_PROC_STATUS || type == INO_PROC_MAPS
        || type == INO_PROC_CMDLINE || type == INO_PROC_ENVIRON;
}

/* ---------------------------------------------------------------------------
 * Tiny string helpers (NUL-terminated kernel strings)
 * ------------------------------------------------------------------------- */

static u32 cstr_len(const char *s) {
    u32 n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static void cstr_copy(char *dst, const char *src, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static u32 name_eq(const char *a, const char *b, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Inode / directory internals
 * ------------------------------------------------------------------------- */

static u32 default_mode_for_type(u32 type) {
    if (type == INO_DIR) {
        return 0x41ED;               /* S_IFDIR | 0755 */
    }
    if (type == INO_SYMLINK) {
        return 0xA1FF;               /* S_IFLNK | 0777 */
    }
    if (type == INO_CONSOLE || type == INO_NULL || type == INO_ZERO) {
        return 0x21B6;               /* S_IFCHR | 0666 */
    }
    /* files + /proc synthetics default to regular 0644 */
    return 0x81A4;
}

static u32 apply_umask_mode(u32 ifmt, u32 create_perm) {
    u32 um = sched_umask_get() & 0x1FF;
    u32 perm = create_perm & 0x1FF;
    return (ifmt & 0xF000) | (perm & ~um);
}

static u32 alloc_ino(u32 type, u32 parent) {
    u32 i;
    /* Slots 2..MAX_INODES-1, skipping fixed console/null/zero slots. */
    for (i = 2; i < MAX_INODES; i++) {
        if (i == CONSOLE_INO || i == NULL_INO || i == ZERO_INO) {
            continue;
        }
        if (inodes[i].type == INO_FREE) {
            u32 k;
            inodes[i].type = type;
            inodes[i].parent = parent;
            inodes[i].ndent = 0;
            inodes[i].ro = 0;
            inodes[i].mode = default_mode_for_type(type);
            inodes[i].sdata = 0;
            inodes[i].buf = 0;
            inodes[i].size = 0;
            inodes[i].cap = 0;
            for (k = 0; k < MAX_DENTS; k++) {
                inodes[i].dents[k].name[0] = 0;
                inodes[i].dents[k].ino = 0;
            }
            return i;
        }
    }
    return 0;
}

static u32 dir_find(u32 dir_ino, const char *name, u32 n) {
    u32 i;
    if (inodes[dir_ino].type != INO_DIR) {
        return 0;
    }
    for (i = 0; i < inodes[dir_ino].ndent; i++) {
        if (name_eq(inodes[dir_ino].dents[i].name, name, n)
            && inodes[dir_ino].dents[i].name[n] == 0) {
            return inodes[dir_ino].dents[i].ino;
        }
    }
    return 0;
}

static u32 dir_add(u32 dir_ino, const char *name, u32 n, u32 child) {
    struct inode *d = &inodes[dir_ino];
    u32 slot;
    if (d->type != INO_DIR || d->ndent >= MAX_DENTS || n == 0 || n > NAME_MAX) {
        return 0;
    }
    if (dir_find(dir_ino, name, n) != 0) {
        return 0;                    /* name exists */
    }
    slot = d->ndent;
    d->dents[slot].name[n] = 0;
    cstr_copy(d->dents[slot].name, name, n);
    d->dents[slot].ino = child;
    d->ndent++;
    return 1;
}

/* Copy symlink target into out (NUL-terminated). Returns byte length or 0. */
static u32 symlink_copy_target(u32 ino, char *out, u32 cap) {
    u64 n;
    u64 i;
    const char *src;
    if (ino == 0 || ino >= MAX_INODES || inodes[ino].type != INO_SYMLINK
        || out == 0 || cap < 2) {
        return 0;
    }
    n = inodes[ino].size;
    if (n == 0 || n > SYMLINK_MAX || n + 1 > cap) {
        return 0;
    }
    src = inodes[ino].buf;
    if (src == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = src[i];
    }
    out[n] = 0;
    return (u32)n;
}

/* Walk components of `path` starting at `cur`. Absolute paths reset to root.
 * Intermediate symlinks are always followed; the final component is followed
 * only when follow_last is set (open/stat/chdir). Depth-limited. */
static u32 walk_lookup_from_flags(u32 cur, const char *path, u32 follow_last,
                                  u32 depth) {
    u32 i = 0;
    if (path == 0 || cur == 0 || cur >= MAX_INODES
        || inodes[cur].type == INO_FREE) {
        return 0;
    }
    if (depth > SYMLOOP_MAX) {
        return 0;
    }
    if (path[0] == '/') {
        cur = ROOT_INO;
    }
    while (path[i] != 0) {
        u32 n;
        u32 nxt;
        u32 is_last;
        u32 j;
        while (path[i] == '/') {
            i++;
        }
        if (path[i] == 0) {
            break;                   /* trailing slash */
        }
        n = 0;
        while (path[i + n] != 0 && path[i + n] != '/') {
            n++;
            if (n > NAME_MAX) {
                return 0;
            }
        }
        j = i + n;
        while (path[j] == '/') {
            j++;
        }
        is_last = (path[j] == 0) ? 1 : 0;
        if (n == 1 && path[i] == '.') {
            /* stay */
        } else if (n == 2 && path[i] == '.' && path[i + 1] == '.') {
            cur = inodes[cur].parent;
        } else {
            nxt = dir_find(cur, &path[i], n);
            if (nxt == 0) {
                return 0;
            }
            if (inodes[nxt].type == INO_SYMLINK
                && (!is_last || follow_last)) {
                char tgt[SYMLINK_MAX + 1];
                u32 start;
                u32 resolved;
                const char *rest;
                if (!symlink_copy_target(nxt, tgt, SYMLINK_MAX + 1)) {
                    return 0;
                }
                start = (tgt[0] == '/') ? ROOT_INO : cur;
                resolved = walk_lookup_from_flags(start, tgt, 1, depth + 1);
                if (resolved == 0) {
                    return 0;
                }
                if (is_last) {
                    return resolved;
                }
                rest = &path[i + n];
                while (rest[0] == '/') {
                    rest++;
                }
                return walk_lookup_from_flags(resolved, rest, follow_last,
                                              depth + 1);
            }
            if (!is_last && inodes[nxt].type != INO_DIR) {
                return 0;            /* intermediate must be a directory */
            }
            cur = nxt;
        }
        i += n;
    }
    return cur;
}

static u32 walk_lookup_from(u32 cur, const char *path) {
    return walk_lookup_from_flags(cur, path, 1, 0);
}

static u32 walk_lookup(const char *path) {
    if (path == 0 || path[0] != '/') {
        return 0;
    }
    return walk_lookup_from_flags(ROOT_INO, path, 1, 0);
}

static u32 walk_lookup_nofollow(const char *path) {
    if (path == 0 || path[0] != '/') {
        return 0;
    }
    return walk_lookup_from_flags(ROOT_INO, path, 0, 0);
}

/* Split path into parent directory + final component, creating missing
 * intermediate directories when ensure_dirs is set. The final component is
 * NOT created; it is returned through last/last_n. Fails when an intermediate
 * component exists but is not a directory, or the name is too long. */
static u32 walk_parent(const char *path, char *last, u32 *last_n, u32 ensure_dirs) {
    u32 cur = ROOT_INO;
    u32 i = 0;
    if (path == 0 || path[0] != '/') {
        return 0;
    }
    while (path[i] != 0) {
        u32 start;
        u32 n;
        while (path[i] == '/') {
            i++;
        }
        if (path[i] == 0) {
            break;
        }
        start = i;
        n = 0;
        while (path[i] != 0 && path[i] != '/') {
            i++;
            n++;
            if (n > NAME_MAX) {
                return 0;
            }
        }
        if (path[i] == 0) {
            /* Final component. */
            *last_n = n;
            cstr_copy(last, &path[start], n);
            last[n] = 0;
            return cur;
        }
        /* Intermediate component: descend, optionally creating directories. */
        if (n == 1 && path[start] == '.') {
            continue;
        }
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            cur = inodes[cur].parent;
            continue;
        }
        {
            u32 nxt = dir_find(cur, &path[start], n);
            if (nxt == 0) {
                if (!ensure_dirs) {
                    return 0;
                }
                nxt = alloc_ino(INO_DIR, cur);
                if (nxt == 0 || !dir_add(cur, &path[start], n, nxt)) {
                    return 0;
                }
            }
            /* Intermediate symlinks must resolve to a directory. */
            if (inodes[nxt].type == INO_SYMLINK) {
                char tgt[SYMLINK_MAX + 1];
                u32 start_ino;
                if (!symlink_copy_target(nxt, tgt, SYMLINK_MAX + 1)) {
                    return 0;
                }
                start_ino = (tgt[0] == '/') ? ROOT_INO : cur;
                nxt = walk_lookup_from_flags(start_ino, tgt, 1, 0);
                if (nxt == 0) {
                    return 0;
                }
            }
            if (inodes[nxt].type != INO_DIR) {
                return 0;
            }
            cur = nxt;
        }
    }
    return 0;                        /* path ended with a slash: no name */
}

/* ---------------------------------------------------------------------------
 * Lifecycle and initial image publishing
 * ------------------------------------------------------------------------- */

void fs_init(void) {
    u32 i;
    for (i = 0; i < MAX_INODES; i++) {
        inodes[i].type = INO_FREE;
        inodes[i].parent = 0;
        inodes[i].ndent = 0;
        inodes[i].ro = 0;
        inodes[i].mode = 0;
        inodes[i].sdata = 0;
        inodes[i].buf = 0;
        inodes[i].size = 0;
        inodes[i].cap = 0;
    }
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        opens[i].used = 0;
        opens[i].ino = 0;
        opens[i].off = 0;
        opens[i].refs = 0;
        opens[i].readable = 0;
        opens[i].writable = 0;
        opens[i].append = 0;
        opens[i].permanent = 0;
    }

    /* Root directory occupies fixed slot ROOT_INO (alloc_ino hands out
     * slots from 2 up); root's parent is itself so "/.." stays at root. */
    inodes[ROOT_INO].type = INO_DIR;
    inodes[ROOT_INO].parent = ROOT_INO;
    inodes[ROOT_INO].mode = 0x41ED;          /* S_IFDIR | 0755 */

    /* Permanent console character device; linked as /dev/tty by
     * fs_publish_chrdevs() after /dev exists. */
    inodes[CONSOLE_INO].type = INO_CONSOLE;
    inodes[CONSOLE_INO].parent = CONSOLE_INO;
    inodes[CONSOLE_INO].mode = 0x21B6;       /* S_IFCHR | 0666 */

    /* Fixed /dev/null and /dev/zero nodes (linked in fs_publish_chrdevs). */
    inodes[NULL_INO].type = INO_NULL;
    inodes[NULL_INO].parent = NULL_INO;
    inodes[NULL_INO].mode = 0x21B6;
    inodes[ZERO_INO].type = INO_ZERO;
    inodes[ZERO_INO].parent = ZERO_INO;
    inodes[ZERO_INO].mode = 0x21B6;

    /* Fixed /proc/self/{status,maps,cmdline,environ}; linked in fs_publish_procfs. */
    inodes[PROC_STATUS_INO].type = INO_PROC_STATUS;
    inodes[PROC_STATUS_INO].mode = 0x81A4;
    inodes[PROC_STATUS_INO].parent = PROC_STATUS_INO;
    inodes[PROC_MAPS_INO].type = INO_PROC_MAPS;
    inodes[PROC_MAPS_INO].mode = 0x81A4;
    inodes[PROC_MAPS_INO].parent = PROC_MAPS_INO;
    inodes[PROC_CMDLINE_INO].type = INO_PROC_CMDLINE;
    inodes[PROC_CMDLINE_INO].mode = 0x81A4;
    inodes[PROC_CMDLINE_INO].parent = PROC_CMDLINE_INO;
    inodes[PROC_ENVIRON_INO].type = INO_PROC_ENVIRON;
    inodes[PROC_ENVIRON_INO].mode = 0x81A4;
    inodes[PROC_ENVIRON_INO].parent = PROC_ENVIRON_INO;

    opens[CONSOLE_FH].used = 1;
    opens[CONSOLE_FH].ino = CONSOLE_INO;
    opens[CONSOLE_FH].permanent = 1;
    opens[CONSOLE_FH].refs = 1;
    opens[CONSOLE_FH].readable = 1;
    opens[CONSOLE_FH].writable = 1;
}

/* Link fixed char devices under /dev (null/zero/tty). /dev must already
 * exist (rootfs_publish creates it). Returns 1 on success. */
u32 fs_publish_chrdevs(void) {
    u32 dev = walk_lookup("/dev");
    if (dev == 0 || inodes[dev].type != INO_DIR) {
        return 0;
    }
    inodes[NULL_INO].parent = dev;
    inodes[ZERO_INO].parent = dev;
    inodes[CONSOLE_INO].parent = dev;
    if (!dir_add(dev, "null", 4, NULL_INO)) {
        return 0;
    }
    if (!dir_add(dev, "zero", 4, ZERO_INO)) {
        return 0;
    }
    if (!dir_add(dev, "tty", 3, CONSOLE_INO)) {
        return 0;
    }
    return 1;
}

/* Link synthetic /proc/self/{status,maps,cmdline,environ}. /proc/self must
 * already exist (rootfs mkdir). Returns 1 on success. */
u32 fs_publish_procfs(void) {
    u32 self = walk_lookup("/proc/self");
    if (self == 0 || inodes[self].type != INO_DIR) {
        return 0;
    }
    inodes[PROC_STATUS_INO].parent = self;
    inodes[PROC_MAPS_INO].parent = self;
    inodes[PROC_CMDLINE_INO].parent = self;
    inodes[PROC_ENVIRON_INO].parent = self;
    if (!dir_add(self, "status", 6, PROC_STATUS_INO)) {
        return 0;
    }
    if (!dir_add(self, "maps", 4, PROC_MAPS_INO)) {
        return 0;
    }
    if (!dir_add(self, "cmdline", 7, PROC_CMDLINE_INO)) {
        return 0;
    }
    if (!dir_add(self, "environ", 7, PROC_ENVIRON_INO)) {
        return 0;
    }
    return 1;
}

/* Publish a static (incbin-backed, read-only) file, creating intermediate
 * directories. Fails on zero length, bad path, inode exhaustion or a name
 * clash. Returns 1 on success. */
u32 fs_publish_file(const char *path, u64 kva, u64 size) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent;
    u32 ino;
    if (kva == 0 || size == 0) {
        return 0;
    }
    parent = walk_parent(path, last, &last_n, 1);
    if (parent == 0 || last_n == 0) {
        return 0;
    }
    ino = alloc_ino(INO_FILE, parent);
    if (ino == 0) {
        return 0;
    }
    inodes[ino].ro = 1;
    inodes[ino].sdata = (const char *)kva;
    inodes[ino].size = size;
    if (!dir_add(parent, last, last_n, ino)) {
        inodes[ino].type = INO_FREE;
        return 0;
    }
    return 1;
}

u32 fs_lookup(const char *path) {
    return walk_lookup(path);
}

/* Like fs_lookup but leave a final symlink unresolved (lstat/readlink). */
u32 fs_lookup_nofollow(const char *path) {
    return walk_lookup_nofollow(path);
}

/* Resolve `rel` relative to directory inode `dir_ino` ("" / "." => dir). */
u32 fs_lookup_at_flags(u32 dir_ino, const char *rel, u32 follow_last) {
    if (dir_ino == 0 || dir_ino >= MAX_INODES
        || inodes[dir_ino].type != INO_DIR) {
        return 0;
    }
    if (rel == 0 || rel[0] == 0 || (rel[0] == '.' && rel[1] == 0)) {
        return dir_ino;
    }
    return walk_lookup_from_flags(dir_ino, rel, follow_last, 0);
}

u32 fs_lookup_at(u32 dir_ino, const char *rel) {
    return fs_lookup_at_flags(dir_ino, rel, 1);
}

/* symlink(2): create linkpath -> target. Target is stored verbatim (may be
 * absolute or relative); linkpath must not exist. Returns inode or 0. */
u32 fs_symlink(const char *target, const char *linkpath) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent;
    u32 ino;
    u32 tlen;
    u32 i;
    char *buf;
    if (target == 0 || linkpath == 0) {
        return 0;
    }
    tlen = cstr_len(target);
    if (tlen == 0 || tlen > SYMLINK_MAX) {
        return 0;
    }
    parent = walk_parent(linkpath, last, &last_n, 0);
    if (parent == 0 || last_n == 0) {
        return 0;
    }
    if (dir_find(parent, last, last_n) != 0) {
        return 0;                    /* EEXIST */
    }
    ino = alloc_ino(INO_SYMLINK, parent);
    if (ino == 0) {
        return 0;
    }
    inodes[ino].mode = apply_umask_mode(0xA000, 0x1FF); /* IFLNK|0777 */
    buf = (char *)mem.kmalloc(tlen + 1);
    if (buf == 0) {
        inodes[ino].type = INO_FREE;
        return 0;
    }
    for (i = 0; i < tlen; i++) {
        buf[i] = target[i];
    }
    buf[tlen] = 0;
    inodes[ino].buf = buf;
    inodes[ino].size = tlen;
    inodes[ino].cap = tlen + 1;
    if (!dir_add(parent, last, last_n, ino)) {
        mem.kfree(buf);
        inodes[ino].buf = 0;
        inodes[ino].type = INO_FREE;
        return 0;
    }
    return ino;
}

/* readlink(2): copy target of the final symlink (nofollow) into kbuf.
 * Returns byte count (no NUL) or (u64)-1. Truncates to bufsiz. */
u64 fs_readlink(const char *path, char *kbuf, u64 bufsiz) {
    u32 ino;
    u64 n;
    u64 i;
    const char *src;
    if (path == 0 || kbuf == 0 || bufsiz == 0) {
        return (u64)(-1);
    }
    ino = walk_lookup_nofollow(path);
    if (ino == 0 || inodes[ino].type != INO_SYMLINK || inodes[ino].buf == 0) {
        return (u64)(-1);
    }
    n = inodes[ino].size;
    if (n > bufsiz) {
        n = bufsiz;
    }
    src = inodes[ino].buf;
    for (i = 0; i < n; i++) {
        kbuf[i] = src[i];
    }
    return n;
}

/* Create a dynamic writable file (fail if the name already exists). */
u32 fs_create(const char *path) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent = walk_parent(path, last, &last_n, 0);
    u32 ino;
    if (parent == 0 || last_n == 0) {
        return 0;
    }
    ino = alloc_ino(INO_FILE, parent);
    if (ino == 0) {
        return 0;
    }
    inodes[ino].mode = apply_umask_mode(0x8000, 0x1B6); /* IFREG|0666 */
    if (!dir_add(parent, last, last_n, ino)) {
        inodes[ino].type = INO_FREE;
        return 0;
    }
    return ino;
}

u32 fs_mkdir(const char *path) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent = walk_parent(path, last, &last_n, 0);
    u32 ino;
    if (parent == 0 || last_n == 0) {
        return 0;
    }
    ino = alloc_ino(INO_DIR, parent);
    if (ino == 0) {
        return 0;
    }
    inodes[ino].mode = apply_umask_mode(0x4000, 0x1FF); /* IFDIR|0777 */
    if (!dir_add(parent, last, last_n, ino)) {
        inodes[ino].type = INO_FREE;
        return 0;
    }
    return ino;
}

/* True when an open-file description references the inode. */
static u32 inode_is_open(u32 ino) {
    u32 i;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (opens[i].used && opens[i].ino == ino) {
            return 1;
        }
    }
    return 0;
}

/* unlink: detach the directory entry. The inode slot is freed only when no
 * open file still references it (POSIX keeps unlinked-but-open files alive);
 * otherwise it lingers detached until reboot, bounded by the slot count. */
/* Remove an empty directory. Non-empty or non-dir targets fail. */
u32 fs_rmdir(const char *path) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent = walk_parent(path, last, &last_n, 0);
    u32 target;
    u32 slot;
    u32 k;
    if (parent == 0 || inodes[parent].type != INO_DIR || last_n == 0) {
        return 0;
    }
    target = dir_find(parent, last, last_n);
    if (target == 0 || inodes[target].type != INO_DIR) {
        return 0;
    }
    if (inodes[target].ndent != 0) {
        return 0;                      /* not empty */
    }
    if (inode_is_open(target)) {
        return 0;
    }
    for (slot = 0; slot < inodes[parent].ndent; slot++) {
        if (inodes[parent].dents[slot].ino == target) {
            break;
        }
    }
    if (slot >= inodes[parent].ndent) {
        return 0;
    }
    for (k = slot; k + 1 < inodes[parent].ndent; k++) {
        inodes[parent].dents[k] = inodes[parent].dents[k + 1];
    }
    inodes[parent].ndent--;
    inodes[target].type = INO_FREE;
    inodes[target].parent = 0;
    inodes[target].ndent = 0;
    return 1;
}

u32 fs_unlink(const char *path) {
    char last[NAME_LEN];
    u32 last_n = 0;
    u32 parent = walk_parent(path, last, &last_n, 0);
    struct inode *d;
    u32 target;
    u32 slot;
    u32 k;

    if (parent == 0 || inodes[parent].type != INO_DIR || last_n == 0) {
        return 0;
    }
    d = &inodes[parent];
    target = dir_find(parent, last, last_n);
    if (target == 0) {
        return 0;
    }
    if (inodes[target].type == INO_DIR) {
        return 0;                    /* use rmdir(2) */
    }
    /* Never unlink fixed character devices. */
    if (target == CONSOLE_INO || target == NULL_INO || target == ZERO_INO
        || target == PROC_STATUS_INO || target == PROC_MAPS_INO
        || target == PROC_CMDLINE_INO || target == PROC_ENVIRON_INO
        || inodes[target].type == INO_CONSOLE
        || inodes[target].type == INO_NULL
        || inodes[target].type == INO_ZERO
        || inode_is_proc(inodes[target].type)) {
        return 0;
    }
    for (slot = 0; slot < d->ndent; slot++) {
        if (d->dents[slot].ino == target) {
            break;
        }
    }
    if (slot >= d->ndent) {
        return 0;
    }
    /* Shift later entries down over the removed slot. */
    for (k = slot; k + 1 < d->ndent; k++) {
        d->dents[k] = d->dents[k + 1];
    }
    d->ndent--;

    if (!inode_is_open(target)) {
        if (inodes[target].buf != 0 && !inodes[target].ro) {
            mem.kfree(inodes[target].buf);
            inodes[target].buf = 0;
            inodes[target].cap = 0;
            inodes[target].size = 0;
        }
        inodes[target].type = INO_FREE;
        inodes[target].parent = 0;
        inodes[target].ndent = 0;
    }
    return 1;
}

/* rename(2) within Ramfs: move a directory entry. Replaces an existing
 * regular file or empty directory at newpath. Directories update parent. */
u32 fs_rename(const char *oldpath, const char *newpath) {
    char olast[NAME_LEN];
    char nlast[NAME_LEN];
    u32 olast_n = 0;
    u32 nlast_n = 0;
    u32 oparent;
    u32 nparent;
    u32 old_ino;
    u32 dest;
    u32 slot;
    u32 k;

    oparent = walk_parent(oldpath, olast, &olast_n, 0);
    nparent = walk_parent(newpath, nlast, &nlast_n, 0);
    if (oparent == 0 || nparent == 0 || olast_n == 0 || nlast_n == 0) {
        return 0;
    }
    if (inodes[oparent].type != INO_DIR || inodes[nparent].type != INO_DIR) {
        return 0;
    }
    if (olast_n == 1 && olast[0] == '.') return 0;
    if (olast_n == 2 && olast[0] == '.' && olast[1] == '.') return 0;
    if (nlast_n == 1 && nlast[0] == '.') return 0;
    if (nlast_n == 2 && nlast[0] == '.' && nlast[1] == '.') return 0;

    if (oparent == nparent && olast_n == nlast_n) {
        u32 same = 1;
        for (k = 0; k < olast_n; k++) {
            if (olast[k] != nlast[k]) { same = 0; break; }
        }
        if (same) return 1;
    }

    old_ino = dir_find(oparent, olast, olast_n);
    if (old_ino == 0) return 0;
    if (old_ino == ROOT_INO || old_ino == CONSOLE_INO
        || old_ino == NULL_INO || old_ino == ZERO_INO) return 0;
    if (inodes[old_ino].type == INO_NULL || inodes[old_ino].type == INO_ZERO
        || inodes[old_ino].type == INO_CONSOLE
        || inode_is_proc(inodes[old_ino].type)) return 0;

    if (inodes[old_ino].type == INO_DIR) {
        u32 cur = nparent;
        u32 guard = 0;
        if (nparent == old_ino) return 0;
        while (cur != ROOT_INO && cur != 0 && guard < 64) {
            if (cur == old_ino) return 0;
            cur = inodes[cur].parent;
            guard++;
        }
    }

    dest = dir_find(nparent, nlast, nlast_n);
    if (dest != 0) {
        if (dest == old_ino) return 1;
        if (inodes[old_ino].type == INO_DIR) {
            if (inodes[dest].type != INO_DIR || inodes[dest].ndent != 0) {
                return 0;
            }
            if (inode_is_open(dest)) return 0;
            /* remove empty dest dir entry */
            for (slot = 0; slot < inodes[nparent].ndent; slot++) {
                if (inodes[nparent].dents[slot].ino == dest) break;
            }
            if (slot >= inodes[nparent].ndent) return 0;
            for (k = slot; k + 1 < inodes[nparent].ndent; k++) {
                inodes[nparent].dents[k] = inodes[nparent].dents[k + 1];
            }
            inodes[nparent].ndent--;
            inodes[dest].type = INO_FREE;
            inodes[dest].parent = 0;
            inodes[dest].ndent = 0;
        } else {
            if (inodes[dest].type == INO_DIR) return 0;
            /* unlink dest file name; free inode if not open */
            for (slot = 0; slot < inodes[nparent].ndent; slot++) {
                if (inodes[nparent].dents[slot].ino == dest) break;
            }
            if (slot >= inodes[nparent].ndent) return 0;
            for (k = slot; k + 1 < inodes[nparent].ndent; k++) {
                inodes[nparent].dents[k] = inodes[nparent].dents[k + 1];
            }
            inodes[nparent].ndent--;
            if (!inode_is_open(dest)) {
                inodes[dest].type = INO_FREE;
                inodes[dest].parent = 0;
                inodes[dest].ndent = 0;
            }
        }
    }

    if (!dir_add(nparent, nlast, nlast_n, old_ino)) return 0;

    /* Remove old directory entry. */
    for (slot = 0; slot < inodes[oparent].ndent; slot++) {
        if (inodes[oparent].dents[slot].ino == old_ino
            && name_eq(inodes[oparent].dents[slot].name, olast, olast_n)
            && inodes[oparent].dents[slot].name[olast_n] == 0) {
            break;
        }
    }
    if (slot >= inodes[oparent].ndent) return 0;
    for (k = slot; k + 1 < inodes[oparent].ndent; k++) {
        inodes[oparent].dents[k] = inodes[oparent].dents[k + 1];
    }
    inodes[oparent].ndent--;
    inodes[old_ino].parent = nparent;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Open file table
 * ------------------------------------------------------------------------- */

static u32 alloc_open(u32 ino, u32 readable, u32 writable, u32 append, u64 off) {
    u32 i;
    for (i = 2; i < MAX_OPEN_FILES; i++) {
        if (!opens[i].used) {
            opens[i].used = 1;
            opens[i].ino = ino;
            opens[i].off = off;
            opens[i].refs = 1;
            opens[i].readable = readable;
            opens[i].writable = writable;
            opens[i].append = append;
            opens[i].permanent = 0;
            opens[i].snap = 0;
            opens[i].snap_len = 0;
            return i;
        }
    }
    return FH_BAD;
}

u32 fs_console_fh(void) {
    return CONSOLE_FH;
}

u32 fs_file_is_console(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    return inodes[opens[fh].ino].type == INO_CONSOLE;
}

u32 fs_file_is_dir(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    return inodes[opens[fh].ino].type == INO_DIR;
}

/* Only regular files may back VMA_FILE mmap (not console/null/zero). */
u32 fs_file_is_mmapable(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    return inodes[opens[fh].ino].type == INO_FILE;
}

u32 fs_file_ino(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    return opens[fh].ino;
}

/* Extra reference from a fork-inherited descriptor. The permanent console
 * slot is shared implicitly. */
void fs_file_retain(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used || opens[fh].permanent) {
        return;
    }
    opens[fh].refs++;
}

u32 fs_file_open(const char *path, u32 flags) {
    return fs_file_open_mode(path, flags, 0x1B6); /* default creat 0666 */
}

u32 fs_file_open_mode(const char *path, u32 flags, u32 creat_mode) {
    u32 ino = walk_lookup(path);
    u32 mode = flags & 3;
    u32 readable = 0;
    u32 writable = 0;
    u64 off = 0;

    if (ino == 0) {
        if ((flags & O_CREAT) == 0) {
            return FH_BAD;
        }
        ino = fs_create(path);
        if (ino == 0) {
            return FH_BAD;
        }
        /* fs_create applied 0666&~umask; honor open(2) mode when non-zero. */
        if (creat_mode != 0) {
            inodes[ino].mode = apply_umask_mode(0x8000, creat_mode);
        }
    } else if ((flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
        return FH_BAD;                /* O_CREAT|O_EXCL and name exists */
    }

    /* /dev/tty opens the console inode via a normal fh (stdin/out/err keep
     * the permanent CONSOLE_FH). Other char devices open the same way. */
    if (inodes[ino].type == INO_DIR && mode != O_RDONLY) {
        return FH_BAD;                /* directories are opened read-only */
    }
    if (inodes[ino].type != INO_FILE && inodes[ino].type != INO_DIR
        && inodes[ino].type != INO_CONSOLE && inodes[ino].type != INO_NULL
        && inodes[ino].type != INO_ZERO && !inode_is_proc(inodes[ino].type)) {
        return FH_BAD;
    }
    /* /proc/self/* are read-only snapshots. */
    if (inode_is_proc(inodes[ino].type) && mode != O_RDONLY) {
        return FH_BAD;
    }
    readable = (mode != O_WRONLY);
    writable = (mode == O_WRONLY || mode == O_RDWR);
    if (writable && inodes[ino].ro) {
        return FH_BAD;                /* static files are immutable */
    }
    if ((flags & O_TRUNC) && writable && inodes[ino].type == INO_FILE) {
        inodes[ino].size = 0;
    }
    if ((flags & O_APPEND) && inodes[ino].type == INO_FILE) {
        off = inodes[ino].size;
    }
    {
        u32 fh = alloc_open(ino, readable, writable, (flags & O_APPEND) != 0, off);
        if (fh == FH_BAD) {
            return FH_BAD;
        }
        if (inode_is_proc(inodes[ino].type)) {
            enum { PROC_SNAP_CAP = 4096 };
            char *buf = (char *)mem.kmalloc(PROC_SNAP_CAP);
            u64 n = 0;
            if (buf == 0) {
                opens[fh].used = 0;
                opens[fh].refs = 0;
                return FH_BAD;
            }
            if (inodes[ino].type == INO_PROC_STATUS) {
                n = procfs_gen_status(buf, PROC_SNAP_CAP);
            } else if (inodes[ino].type == INO_PROC_MAPS) {
                n = procfs_gen_maps(buf, PROC_SNAP_CAP);
            } else if (inodes[ino].type == INO_PROC_CMDLINE) {
                n = procfs_gen_cmdline(buf, PROC_SNAP_CAP);
            } else {
                n = procfs_gen_environ(buf, PROC_SNAP_CAP);
            }
            opens[fh].snap = buf;
            opens[fh].snap_len = n;
        }
        return fh;
    }
}

u32 fs_close(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    if (opens[fh].permanent) {
        return 1;                     /* console: nothing to release */
    }
    if (opens[fh].refs > 1) {
        opens[fh].refs--;
        return 1;
    }
    if (opens[fh].snap != 0) {
        mem.kfree(opens[fh].snap);
        opens[fh].snap = 0;
        opens[fh].snap_len = 0;
    }
    opens[fh].used = 0;
    opens[fh].refs = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * File data path
 * ------------------------------------------------------------------------- */

static u32 ensure_cap(struct inode *ip, u64 need) {
    u64 cap;
    char *nb;
    u64 i;
    if (need <= ip->cap) {
        return 1;
    }
    if (ip->ro || need > FILE_MAX) {
        return 0;
    }
    cap = ip->cap != 0 ? ip->cap : 4096;
    while (cap < need) {
        cap <<= 1;
        if (cap >= FILE_MAX) {
            cap = FILE_MAX;
            break;
        }
    }
    if (cap < need) {
        return 0;
    }
    nb = (char *)mem.kmalloc(cap);
    if (nb == 0) {
        return 0;
    }
    for (i = 0; i < ip->size; i++) {
        nb[i] = ip->buf != 0 ? ip->buf[i] : 0;
    }
    if (ip->buf != 0) {
        mem.kfree(ip->buf);
    }
    ip->buf = nb;
    ip->cap = cap;
    return 1;
}

u64 fs_read_h(u32 fh, char *kbuf, u64 len) {
    struct inode *ip;
    u64 n;
    u64 i;
    const char *src;
    if (fh >= MAX_OPEN_FILES || !opens[fh].used || kbuf == 0) {
        return (u64)(-1);
    }
    ip = &inodes[opens[fh].ino];
    if (!opens[fh].readable) {
        return (u64)(-1);
    }
    if (ip->type == INO_NULL) {
        return 0;                     /* always EOF */
    }
    if (ip->type == INO_ZERO) {
        for (i = 0; i < len; i++) {
            kbuf[i] = 0;
        }
        return len;
    }
    /* /proc/self snapshot taken at open. */
    if (opens[fh].snap != 0) {
        if (opens[fh].off >= opens[fh].snap_len) {
            return 0;
        }
        n = opens[fh].snap_len - opens[fh].off;
        if (n > len) {
            n = len;
        }
        src = opens[fh].snap;
        for (i = 0; i < n; i++) {
            kbuf[i] = src[opens[fh].off + i];
        }
        opens[fh].off += n;
        return n;
    }
    if (ip->type != INO_FILE) {
        return (u64)(-1);             /* console uses tty path; dirs use getdents */
    }
    if (opens[fh].off >= ip->size) {
        return 0;
    }
    n = ip->size - opens[fh].off;
    if (n > len) {
        n = len;
    }
    src = ip->ro ? ip->sdata : ip->buf;
    for (i = 0; i < n; i++) {
        kbuf[i] = src[opens[fh].off + i];
    }
    opens[fh].off += n;
    return n;
}

u64 fs_write_h(u32 fh, const char *kbuf, u64 len) {
    struct inode *ip;
    u64 need;
    u64 i;
    if (fh >= MAX_OPEN_FILES || !opens[fh].used || kbuf == 0) {
        return (u64)(-1);
    }
    ip = &inodes[opens[fh].ino];
    if (!opens[fh].writable) {
        return (u64)(-1);
    }
    if (ip->type == INO_NULL || ip->type == INO_ZERO) {
        return len;                   /* discard */
    }
    if (ip->type != INO_FILE || ip->ro) {
        return (u64)(-1);
    }
    /* O_APPEND forces every write to EOF, even after an intervening lseek. */
    if (opens[fh].append) {
        opens[fh].off = ip->size;
    }
    need = opens[fh].off + len;
    if (need < opens[fh].off || need > FILE_MAX) {
        return (u64)(-1);             /* overflow / ceiling */
    }
    if (!ensure_cap(ip, need)) {
        return (u64)(-1);
    }
    /* Sparse tail between EOF and the write offset is zero-filled. */
    for (i = ip->size; i < opens[fh].off; i++) {
        ip->buf[i] = 0;
    }
    for (i = 0; i < len; i++) {
        ip->buf[opens[fh].off + i] = kbuf[i];
    }
    opens[fh].off += len;
    if (opens[fh].off > ip->size) {
        ip->size = opens[fh].off;
    }
    return len;
}

u64 fs_lseek_h(u32 fh, u64 off, u32 whence) {
    struct inode *ip;
    u64 no;
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return (u64)(-1);
    }
    ip = &inodes[opens[fh].ino];
    if (ip->type == INO_CONSOLE) {
        return (u64)(-1);
    }
    if (ip->type == INO_NULL || ip->type == INO_ZERO) {
        /* Offset is ignored on I/O; still track it for POSIX lseek. */
        if (whence == SEEK_SET) {
            no = off;
        } else if (whence == SEEK_CUR) {
            no = opens[fh].off + off;
        } else if (whence == SEEK_END) {
            no = off;                 /* size is always 0 */
        } else {
            return (u64)(-1);
        }
        opens[fh].off = no;
        return no;
    }
    if (opens[fh].snap != 0) {
        if (whence == SEEK_SET) {
            no = off;
        } else if (whence == SEEK_CUR) {
            no = opens[fh].off + off;
        } else if (whence == SEEK_END) {
            no = opens[fh].snap_len + off;
        } else {
            return (u64)(-1);
        }
        opens[fh].off = no;
        return no;
    }
    if (whence == SEEK_SET) {
        no = off;
    } else if (whence == SEEK_CUR) {
        no = opens[fh].off + off;
    } else if (whence == SEEK_END) {
        no = ip->size + off;
    } else {
        return (u64)(-1);
    }
    if (ip->type == INO_FILE && no > FILE_MAX) {
        return (u64)(-1);
    }
    opens[fh].off = no;
    return no;
}

/* Emit fixed 48-byte dirent records:
 *   0..7   d_ino   (u64)
 *   8..15  d_off   (u64, index of the NEXT entry)
 *   16..17 d_reclen(u16 = 48)
 *   18     d_type  (u8: DT_REG / DT_DIR)
 *   19..47 d_name  (29 bytes, NUL padded)
 * Records are assembled bytewise so callers need no alignment guarantees. */
static void put_u64(char *p, u64 v) {
    u32 i;
    for (i = 0; i < 8; i++) {
        p[i] = (char)(v >> (i * 8));
    }
}

u64 fs_getdents_h(u32 fh, char *kbuf, u64 len) {
    struct inode *dp;
    u64 produced = 0;
    u32 idx;
    if (fh >= MAX_OPEN_FILES || !opens[fh].used || kbuf == 0) {
        return (u64)(-1);
    }
    dp = &inodes[opens[fh].ino];
    if (dp->type != INO_DIR || !opens[fh].readable) {
        return (u64)(-1);
    }
    if (len < DENT_RECLEN) {
        return (u64)(-1);             /* caller cannot distinguish from EOF */
    }
    idx = (u32)opens[fh].off;
    while (idx < dp->ndent && produced + DENT_RECLEN <= len) {
        struct dent *de = &dp->dents[idx];
        char *r = kbuf + produced;
        u32 k;
        for (k = 0; k < DENT_RECLEN; k++) {
            r[k] = 0;
        }
        put_u64(r, de->ino);
        put_u64(r + 8, (u64)idx + 1);
        r[16] = (char)(DENT_RECLEN & 0xFF);
        r[17] = (char)((DENT_RECLEN >> 8) & 0xFF);
        if (inodes[de->ino].type == INO_DIR) {
            r[18] = (char)DT_DIR;
        } else if (inodes[de->ino].type == INO_SYMLINK) {
            r[18] = (char)DT_LNK;
        } else if (inodes[de->ino].type == INO_FILE
                   || inode_is_proc(inodes[de->ino].type)) {
            r[18] = (char)DT_REG;
        } else {
            r[18] = (char)DT_CHR;     /* console/null/zero */
        }
        for (k = 0; k < NAME_LEN && de->name[k] != 0 && k < 29; k++) {
            r[19 + k] = de->name[k];
        }
        produced += DENT_RECLEN;
        idx++;
    }
    opens[fh].off = idx;
    return produced;
}

/* ---------------------------------------------------------------------------
 * Inode-level accessors (used by the execve loader, which maps the ELF
 * directly without an open file)
 * ------------------------------------------------------------------------- */


/* Absolute-offset write into a writable (non-ro) file inode. Grows size. */
u64 fs_ino_pwrite(u32 ino, u64 off, const char *kbuf, u64 len) {
    struct inode *ip;
    u64 need;
    u64 i;
    if (ino == 0 || ino >= MAX_INODES || kbuf == 0 || len == 0) {
        return (u64)(-1);
    }
    ip = &inodes[ino];
    if (ip->type != INO_FILE || ip->ro) {
        return (u64)(-1);
    }
    need = off + len;
    if (need < off || need > FILE_MAX) {
        return (u64)(-1);
    }
    if (!ensure_cap(ip, need)) {
        return (u64)(-1);
    }
    for (i = ip->size; i < off; i++) {
        ip->buf[i] = 0;
    }
    for (i = 0; i < len; i++) {
        ip->buf[off + i] = kbuf[i];
    }
    if (off + len > ip->size) {
        ip->size = off + len;
    }
    return len;
}

/* Resize a writable dynamic file (POSIX ftruncate). Grow zero-fills;
 * shrink only updates size (capacity retained). */
u32 fs_ftruncate_h(u32 fh, u64 size) {
    struct inode *ip;
    u64 i;
    if (fh >= MAX_OPEN_FILES || !opens[fh].used) {
        return 0;
    }
    if (!opens[fh].writable) {
        return 0;
    }
    ip = &inodes[opens[fh].ino];
    if (ip->type != INO_FILE || ip->ro) {
        return 0;
    }
    if (size > FILE_MAX) {
        return 0;
    }
    if (size > ip->size) {
        if (!ensure_cap(ip, size)) {
            return 0;
        }
        for (i = ip->size; i < size; i++) {
            ip->buf[i] = 0;
        }
    }
    ip->size = size;
    return 1;
}

u32 fs_ino_kind(u32 ino) {
    if (ino == 0 || ino >= MAX_INODES || inodes[ino].type == INO_FREE) {
        return INO_FREE;
    }
    return inodes[ino].type;
}

u64 fs_ino_size(u32 ino) {
    if (ino == 0 || ino >= MAX_INODES) {
        return 0;
    }
    return inodes[ino].size;
}

u64 fs_ino_data(u32 ino) {
    if (ino == 0 || ino >= MAX_INODES) {
        return 0;
    }
    if (inodes[ino].ro) {
        return (u64)inodes[ino].sdata;
    }
    return (u64)inodes[ino].buf;
}

/* Debugging/statistics for the boot log. */
u32 fs_ino_count(void) {
    u32 n = 0;
    u32 i;
    for (i = 1; i < MAX_INODES; i++) {
        if (inodes[i].type != INO_FREE) {
            n++;
        }
    }
    return n;
}


/* chmod(2): replace permission bits (07777), keep S_IFMT. */
u32 fs_chmod(const char *path, u32 mode) {
    u32 ino = walk_lookup_nofollow(path);
    u32 cur;
    if (ino == 0) {
        return 0;
    }
    cur = inodes[ino].mode;
    if (cur == 0) {
        cur = default_mode_for_type(inodes[ino].type);
    }
    inodes[ino].mode = (cur & 0xF000) | (mode & 0x0FFF);
    return 1;
}

/* Fill Linux-ish mode/size/rdev for a ramfs inode. Returns 0 on failure.
 * mode already includes S_IFxxx | permission bits; rdev is major/minor for
 * char devices (null/zero/tty), else 0. */
u32 fs_stat_ino(u32 ino, u32 *mode_out, u64 *size_out, u32 *rdev_out,
                u32 *nlink_out) {
    u32 mode;
    u32 rdev;
    u32 nlink;
    u64 size;
    u32 t;
    if (ino == 0 || ino >= MAX_INODES) {
        return 0;
    }
    t = inodes[ino].type;
    if (t == INO_FREE) {
        return 0;
    }
    size = inodes[ino].size;
    rdev = 0;
    nlink = 1;
    mode = inodes[ino].mode;
    if (mode == 0) {
        mode = default_mode_for_type(t);
    }
    if (t == INO_DIR) {
        nlink = inodes[ino].ndent + 2;
        size = 0;
    } else if (t == INO_CONSOLE) {
        rdev = (5 << 8) | 0;         /* tty-ish */
        size = 0;
    } else if (t == INO_NULL) {
        rdev = (1 << 8) | 3;         /* /dev/null */
        size = 0;
    } else if (t == INO_ZERO) {
        rdev = (1 << 8) | 5;         /* /dev/zero */
        size = 0;
    } else if (inode_is_proc(t)) {
        size = 0;                    /* live size known after open snapshot */
    } else if (t != INO_FILE && t != INO_SYMLINK) {
        return 0;
    }
    if (mode_out) {
        *mode_out = mode;
    }
    if (size_out) {
        *size_out = size;
    }
    if (rdev_out) {
        *rdev_out = rdev;
    }
    if (nlink_out) {
        *nlink_out = nlink;
    }
    return 1;
}
