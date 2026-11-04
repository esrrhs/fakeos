/* fakeos userland - tr: translate or delete characters (thin POSIX subset).
 * Forms: tr SET1 SET2 | tr -d SET1 | tr -c SET1 SET2 | tr -cd SET1
 * -c/-C complement SET1. Glued -cd/-dc/-c accepted.
 * Sets: literals, ranges a-z, and \\ \n \t \r \0 escapes. stdin -> stdout.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern int strcmp(const char *a, const char *b);

enum { SET_MAX = 512 };

static void usage(void) {
    u_write(2, "usage: tr [-c] SET1 SET2 | tr -d[-c] SET1\n", 44);
}

/* Expand SET into out[]; return count, or -1 on error. */
static int expand_set(const char *set, unsigned char *out, int cap) {
    int i = 0;
    int n = 0;
    if (set == 0) return -1;
    while (set[i]) {
        unsigned char a;
        unsigned char b;
        if (n >= cap) return -1;
        if (set[i] == '\\' && set[i + 1] != 0) {
            char e = set[i + 1];
            if (e == 'n') a = '\n';
            else if (e == 't') a = '\t';
            else if (e == 'r') a = '\r';
            else if (e == '0') a = 0;
            else if (e == '\\') a = '\\';
            else a = (unsigned char)e;
            i += 2;
        } else {
            a = (unsigned char)set[i];
            i++;
        }
        /* Range: X-Y when next is '-' and a following char exists. */
        if (set[i] == '-' && set[i + 1] != 0) {
            i++; /* skip '-' */
            if (set[i] == '\\' && set[i + 1] != 0) {
                char e = set[i + 1];
                if (e == 'n') b = '\n';
                else if (e == 't') b = '\t';
                else if (e == 'r') b = '\r';
                else if (e == '0') b = 0;
                else if (e == '\\') b = '\\';
                else b = (unsigned char)e;
                i += 2;
            } else {
                b = (unsigned char)set[i];
                i++;
            }
            if (b < a) return -1;
            while (a <= b) {
                if (n >= cap) return -1;
                out[n++] = a;
                if (a == 255) break;
                a++;
            }
        } else {
            out[n++] = a;
        }
    }
    return n;
}

/* Build complement of set1[0..n1) into out[]; return count (always 256-unique). */
static int complement_set(const unsigned char *set1, int n1,
                          unsigned char *out, int cap) {
    unsigned char in_set[256];
    int k;
    int n = 0;
    for (k = 0; k < 256; k++) in_set[k] = 0;
    for (k = 0; k < n1; k++) in_set[set1[k]] = 1;
    for (k = 0; k < 256; k++) {
        if (!in_set[k]) {
            if (n >= cap) return -1;
            out[n++] = (unsigned char)k;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    int delete_mode = 0;
    int complement = 0;
    int i = 1;
    unsigned char set1[SET_MAX];
    unsigned char set1c[256];
    unsigned char set2[SET_MAX];
    unsigned char *use1;
    int n1;
    int n1u;
    int n2 = 0;
    unsigned char map[256];
    unsigned char del[256];
    char buf[512];
    long n;
    int k;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'd') delete_mode = 1;
            else if (o == 'c' || o == 'C') complement = 1;
            else {
                usage();
                return 1;
            }
        }
        i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;

    if (delete_mode) {
        if (i + 1 != argc) {
            usage();
            return 1;
        }
        n1 = expand_set(argv[i], set1, SET_MAX);
        if (n1 < 0) {
            usage();
            return 1;
        }
        if (complement) {
            n1u = complement_set(set1, n1, set1c, 256);
            if (n1u < 0) {
                usage();
                return 1;
            }
            use1 = set1c;
            n1 = n1u;
        } else {
            use1 = set1;
        }
        for (k = 0; k < 256; k++) del[k] = 0;
        for (k = 0; k < n1; k++) del[use1[k]] = 1;
        while ((n = u_read(0, buf, sizeof(buf))) > 0) {
            long j;
            for (j = 0; j < n; j++) {
                unsigned char c = (unsigned char)buf[j];
                if (!del[c]) u_write(1, &buf[j], 1);
            }
        }
        return n < 0 ? 1 : 0;
    }

    if (i + 2 != argc) {
        usage();
        return 1;
    }
    n1 = expand_set(argv[i], set1, SET_MAX);
    n2 = expand_set(argv[i + 1], set2, SET_MAX);
    if (n1 < 0 || n2 <= 0) {
        usage();
        return 1;
    }
    if (complement) {
        n1u = complement_set(set1, n1, set1c, 256);
        if (n1u < 0) {
            usage();
            return 1;
        }
        use1 = set1c;
        n1 = n1u;
    } else {
        use1 = set1;
    }
    /* POSIX: if SET2 shorter, reuse its last char. */
    for (k = 0; k < 256; k++) map[k] = (unsigned char)k;
    for (k = 0; k < n1; k++) {
        unsigned char to;
        if (k < n2) to = set2[k];
        else to = set2[n2 - 1];
        map[use1[k]] = to;
    }
    while ((n = u_read(0, buf, sizeof(buf))) > 0) {
        long j;
        for (j = 0; j < n; j++) {
            unsigned char c = map[(unsigned char)buf[j]];
            char outc = (char)c;
            u_write(1, &outc, 1);
        }
    }
    return n < 0 ? 1 : 0;
}
