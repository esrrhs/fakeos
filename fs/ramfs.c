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

/* ---------------------------------------------------------------------------
 * Minimal in-memory VFS + Ramfs
 *
 * Objects:
 *   inode  - file, directory or the console character device
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

    ROOT_INO    = 1,
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
    O_TRUNC   = 0x200,
    O_APPEND  = 0x400,

    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,

    DT_REG = 8,
    DT_DIR = 4
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
};

static struct inode inodes[MAX_INODES];
static struct open_file opens[MAX_OPEN_FILES];

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

static u32 alloc_ino(u32 type, u32 parent) {
    u32 i;
    /* Slots 2..MAX_INODES-1, skipping the fixed console slot. */
    for (i = 2; i < MAX_INODES; i++) {
        if (i == CONSOLE_INO) {
            continue;
        }
        if (inodes[i].type == INO_FREE) {
            u32 k;
            inodes[i].type = type;
            inodes[i].parent = parent;
            inodes[i].ndent = 0;
            inodes[i].ro = 0;
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

/* Absolute path lookup. Empty components (leading/double/trailing slash),
 * "." and ".." are honored. Returns inode number or 0 on any failure. */
static u32 walk_lookup(const char *path) {
    u32 cur = ROOT_INO;
    u32 i = 0;
    if (path == 0 || path[0] != '/') {
        return 0;
    }
    while (path[i] != 0) {
        u32 n;
        u32 nxt;
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
        if (n == 1 && path[i] == '.') {
            /* stay */
        } else if (n == 2 && path[i] == '.' && path[i + 1] == '.') {
            cur = inodes[cur].parent;
        } else {
            nxt = dir_find(cur, &path[i], n);
            if (nxt == 0) {
                return 0;
            }
            cur = nxt;
        }
        i += n;
    }
    return cur;
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

    /* Permanent console character device, never linked into the tree. */
    inodes[CONSOLE_INO].type = INO_CONSOLE;
    inodes[CONSOLE_INO].parent = CONSOLE_INO;

    opens[CONSOLE_FH].used = 1;
    opens[CONSOLE_FH].ino = CONSOLE_INO;
    opens[CONSOLE_FH].permanent = 1;
    opens[CONSOLE_FH].refs = 1;
    opens[CONSOLE_FH].readable = 1;
    opens[CONSOLE_FH].writable = 1;
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
    u32 ino = walk_lookup(path);
    return ino;
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
        inodes[target].type = INO_FREE;
        inodes[target].parent = 0;
        inodes[target].ndent = 0;
    }
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

/* Extra reference from a fork-inherited descriptor. The permanent console
 * slot is shared implicitly. */
void fs_file_retain(u32 fh) {
    if (fh >= MAX_OPEN_FILES || !opens[fh].used || opens[fh].permanent) {
        return;
    }
    opens[fh].refs++;
}

u32 fs_file_open(const char *path, u32 flags) {
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
    }

    if (inodes[ino].type == INO_CONSOLE) {
        return FH_BAD;                /* console is acquired by fixed fh only */
    }
    if (inodes[ino].type == INO_DIR && mode != O_RDONLY) {
        return FH_BAD;                /* directories are opened read-only */
    }
    readable = (mode != O_WRONLY);
    writable = (mode == O_WRONLY || mode == O_RDWR);
    if (writable && inodes[ino].ro) {
        return FH_BAD;                /* static files are immutable */
    }
    if ((flags & O_TRUNC) && writable) {
        inodes[ino].size = 0;
    }
    if (flags & O_APPEND) {
        off = inodes[ino].size;
    }
    return alloc_open(ino, readable, writable, (flags & O_APPEND) != 0, off);
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
    if (ip->type != INO_FILE || !opens[fh].readable) {
        return (u64)(-1);             /* unreadable descriptor / non-file */
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
    if (ip->type != INO_FILE || ip->ro || !opens[fh].writable) {
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
        r[18] = (char)(inodes[de->ino].type == INO_DIR ? DT_DIR : DT_REG);
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
