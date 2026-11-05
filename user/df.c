/* fakeos userland - df: report filesystem disk space (thin).
 *   df [-h] [PATH...]
 * Uses statfs(2). Default PATH: "/". -h: human KiB/MiB/GiB (1024).
 * Columns: Filesystem 1K-blocks Used Available Use% Mounted-on.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_statfs(const char *path, void *buf);
extern void printf(const char *fmt, ...);

enum {
    F_TYPE = 0,
    F_BSIZE = 8,
    F_BLOCKS = 16,
    F_BFREE = 24,
    F_BAVAIL = 32,
    SF_SIZE = 120
};

static int opt_human;

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static void emit_kib(unsigned long kib) {
    if (!opt_human) {
        printf("%u", (unsigned int)kib);
        return;
    }
    if (kib < 1024UL) {
        printf("%uK", (unsigned int)kib);
    } else if (kib < 1024UL * 1024UL) {
        printf("%uM", (unsigned int)(kib / 1024UL));
    } else {
        printf("%uG", (unsigned int)(kib / (1024UL * 1024UL)));
    }
}

static int is_ext2_type(unsigned long t) {
    return t == 0xEF53;
}

static int df_one(const char *path) {
    char sf[SF_SIZE];
    unsigned long type;
    unsigned long bsize;
    unsigned long blocks;
    unsigned long bfree;
    unsigned long bavail;
    unsigned long used_blks;
    unsigned long pct;
    unsigned long i;
    unsigned long kib_tot;
    unsigned long kib_used;
    unsigned long kib_avail;
    const char *fstype;
    const char *mnt;

    for (i = 0; i < SF_SIZE; i++) sf[i] = 0;
    if (u_statfs(path, sf) != 0) {
        printf("df: %s: cannot statfs\n", path);
        return 0;
    }
    type = load_u64(sf + F_TYPE);
    bsize = load_u64(sf + F_BSIZE);
    blocks = load_u64(sf + F_BLOCKS);
    bfree = load_u64(sf + F_BFREE);
    bavail = load_u64(sf + F_BAVAIL);
    if (bsize == 0) bsize = 1024;
    if (bavail > bfree) bavail = bfree;
    used_blks = (blocks > bfree) ? (blocks - bfree) : 0;
    if (blocks == 0) pct = 0;
    else {
        pct = (used_blks * 100UL + blocks - 1) / blocks;
        if (pct > 100) pct = 100;
    }
    kib_tot = (blocks * bsize) / 1024UL;
    kib_used = (used_blks * bsize) / 1024UL;
    kib_avail = (bavail * bsize) / 1024UL;
    fstype = is_ext2_type(type) ? "ext2" : "ramfs";
    if (path[0] == '/' && path[1] == 'm' && path[2] == 'n' && path[3] == 't') {
        mnt = "/mnt";
    } else if (path[0] == '/' && path[1] == 'h' && path[2] == 'o'
               && path[3] == 'm' && path[4] == 'e') {
        mnt = "/home";
    } else if (is_ext2_type(type)) {
        mnt = path;
    } else {
        mnt = "/";
    }
    printf("%s\t", fstype);
    emit_kib(kib_tot);
    printf("\t");
    emit_kib(kib_used);
    printf("\t");
    emit_kib(kib_avail);
    printf("\t%u%%\t%s\n", (unsigned int)pct, mnt);
    return 1;
}

static void usage(void) {
    u_write(2, "usage: df [-h] [PATH...]\n", 25);
}

int main(int argc, char **argv) {
    int ai = 1;
    int i;
    int rc = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'h') opt_human = 1;
            else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    printf("Filesystem\t1K-blocks\tUsed\tAvailable\tUse%%\tMounted on\n");
    if (ai >= argc) {
        if (!df_one("/")) rc = 1;
        return rc;
    }
    for (i = ai; i < argc; i++) {
        if (!df_one(argv[i])) rc = 1;
    }
    return rc;
}
