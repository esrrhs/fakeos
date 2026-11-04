/* fakeos userland - pathchk: check pathnames (thin POSIX).
 * Forms: pathchk [-p] PATH...
 * Default: empty path rejected; total length <= 4095; each component <= 255.
 * -p: POSIX portable charset [A-Za-z0-9._-], _POSIX_PATH_MAX=256,
 *     _POSIX_NAME_MAX=14. No pathconf(2) yet — fixed limits. Compact argv.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    DEF_PATH_MAX = 4095,
    DEF_NAME_MAX = 255,
    POS_PATH_MAX = 256,
    POS_NAME_MAX = 14
};

static int opt_p;

static int is_portable_char(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '.' || c == '_' || c == '-') return 1;
    return 0;
}

static int check_one(const char *path) {
    unsigned long plen;
    unsigned long i;
    unsigned long comp_len;
    unsigned long path_max;
    unsigned long name_max;
    int bad;
    int charset_hit;

    if (path == 0 || path[0] == 0) {
        u_write(2, "pathchk: empty pathname\n", 24);
        return 1;
    }

    plen = strlen(path);
    path_max = opt_p ? (unsigned long)POS_PATH_MAX : (unsigned long)DEF_PATH_MAX;
    name_max = opt_p ? (unsigned long)POS_NAME_MAX : (unsigned long)DEF_NAME_MAX;
    bad = 0;
    charset_hit = 0;

    if (plen > path_max) {
        printf("pathchk: %s: pathname too long\n", path);
        bad = 1;
    }

    comp_len = 0;
    i = 0;
    while (1) {
        char c = path[i];
        if (c == 0 || c == '/') {
            if (comp_len > name_max) {
                printf("pathchk: %s: component too long\n", path);
                bad = 1;
            }
            comp_len = 0;
            if (c == 0) break;
            i++;
            continue;
        }
        if (opt_p && !charset_hit && !is_portable_char(c)) {
            printf("pathchk: %s: nonportable character\n", path);
            bad = 1;
            charset_hit = 1;
        }
        comp_len++;
        i++;
    }

    return bad;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    opt_p = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-p") == 0) {
            opt_p = 1;
            i++;
            continue;
        }
        u_write(2, "usage: pathchk [-p] PATH...\n", 28);
        return 1;
    }

    if (i >= argc) {
        u_write(2, "usage: pathchk [-p] PATH...\n", 28);
        return 1;
    }

    for (; i < argc; i++) {
        if (check_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
