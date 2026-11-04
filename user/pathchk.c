/* fakeos userland - pathchk: check pathnames (thin POSIX).
 * Forms: pathchk [-pP] PATH...
 * Default: empty path rejected; total length <= 4095; each component <= 255.
 * -p: POSIX portable charset [A-Za-z0-9._-], _POSIX_PATH_MAX=256,
 *     _POSIX_NAME_MAX=14.
 * -P: reject empty components (e.g. a//b) and components that begin with '-';
 *     alone, -P skips the default length checks (POSIX); with -p, both apply.
 * No pathconf(2) yet — fixed limits. Compact argv for EXEC_MAX_ARGC=8.
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
static int opt_P;

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
    int do_len;
    int empty_hit;
    int hyphen_hit;

    if (path == 0 || path[0] == 0) {
        u_write(2, "pathchk: empty pathname\n", 24);
        return 1;
    }

    plen = strlen(path);
    bad = 0;
    charset_hit = 0;
    empty_hit = 0;
    hyphen_hit = 0;

    /* Length checks: default, or -p. Alone -P replaces them (POSIX). */
    do_len = opt_p || !opt_P;
    if (do_len) {
        path_max = opt_p ? (unsigned long)POS_PATH_MAX : (unsigned long)DEF_PATH_MAX;
        name_max = opt_p ? (unsigned long)POS_NAME_MAX : (unsigned long)DEF_NAME_MAX;
        if (plen > path_max) {
            printf("pathchk: %s: pathname too long\n", path);
            bad = 1;
        }
    } else {
        path_max = 0;
        name_max = 0;
    }

    /* Walk components. Leading absolute '/' is not an empty component. */
    i = 0;
    if (path[0] == '/') {
        while (path[i] == '/') i++;
    }
    while (path[i] != 0) {
        char c0;
        if (path[i] == '/') {
            /* consecutive slash => empty component */
            if (opt_P && !empty_hit) {
                printf("pathchk: %s: empty component\n", path);
                bad = 1;
                empty_hit = 1;
            }
            while (path[i] == '/') i++;
            continue;
        }
        c0 = path[i];
        if (opt_P && !hyphen_hit && c0 == '-') {
            printf("pathchk: %s: component starts with '-'\n", path);
            bad = 1;
            hyphen_hit = 1;
        }
        comp_len = 0;
        while (path[i] != 0 && path[i] != '/') {
            char c = path[i];
            if (opt_p && !charset_hit && !is_portable_char(c)) {
                printf("pathchk: %s: nonportable character\n", path);
                bad = 1;
                charset_hit = 1;
            }
            comp_len++;
            i++;
        }
        if (do_len && comp_len > name_max) {
            printf("pathchk: %s: component too long\n", path);
            bad = 1;
        }
        /* trailing slash leaves an empty component after this name */
        if (path[i] == '/') {
            unsigned long j = i;
            while (path[j] == '/') j++;
            if (path[j] == 0) {
                if (opt_P && !empty_hit) {
                    printf("pathchk: %s: empty component\n", path);
                    bad = 1;
                    empty_hit = 1;
                }
                break;
            }
        }
    }

    return bad;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    opt_p = 0;
    opt_P = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        int j;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[1] == 0) break;
        for (j = 1; a[j]; j++) {
            if (a[j] == 'p') opt_p = 1;
            else if (a[j] == 'P') opt_P = 1;
            else {
                u_write(2, "usage: pathchk [-pP] PATH...\n", 29);
                return 1;
            }
        }
        i++;
    }

    if (i >= argc) {
        u_write(2, "usage: pathchk [-pP] PATH...\n", 29);
        return 1;
    }

    for (; i < argc; i++) {
        if (check_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
