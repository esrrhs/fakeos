/* fakeos userland - sed: thin stream editor (s///g + d/p/q).
 * Forms: sed [-n] SCRIPT [FILE...]
 * SCRIPT: semicolon-separated commands (no addresses):
 *   s/PAT/REPL/[g]  fixed-string substitute (first match, or all with g)
 *   d               delete line (suppress default print)
 *   p               print pattern space
 *   q               quit after this line
 * Delimiter after s may be any non-NUL (e.g. s#a#b#). PAT/REPL: literals plus
 * \\ \n \t escapes. -n suppresses default print. stdin if no FILE; "-" stdin.
 * Compact argv for EXEC_MAX_ARGC=8. No regex, no addresses, no hold space.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { LINE_MAX = 512, OUT_MAX = 1024 };

static int quiet; /* -n */
static int quit_flag;

static void usage(void) {
    u_write(2, "usage: sed [-n] SCRIPT [FILE...]\n", 33);
}

/* Decode one escape at s[*si] into *outc; advance *si. Returns 0 on bare '\\'. */
static int take_esc(const char *s, int *si, char *outc) {
    int i = *si;
    if (s[i] == '\\' && s[i + 1] != 0) {
        char e = s[i + 1];
        if (e == 'n') *outc = '\n';
        else if (e == 't') *outc = '\t';
        else if (e == '\\') *outc = '\\';
        else *outc = e;
        *si = i + 2;
        return 1;
    }
    if (s[i] == '\\') return 0;
    *outc = s[i];
    *si = i + 1;
    return 1;
}

/* First index of needle[0..nlen) in hay[0..hlen), or -1. */
static int find_sub(const char *hay, int hlen, const char *needle, int nlen) {
    int i;
    int j;
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (i = 0; i + nlen <= hlen; i++) {
        int ok = 1;
        for (j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) return i;
    }
    return -1;
}

/* script[off..] is text after 's'. Sets *newoff past the command. */
static int do_subst(char *line, int *linelen, const char *script, int off, int *newoff) {
    char delim;
    char pat[LINE_MAX];
    char repl[LINE_MAX];
    char out[OUT_MAX];
    int pn = 0;
    int rn = 0;
    int global = 0;
    int i = off;
    int oi;
    int li;
    int left;
    int at;
    int nlen;
    int rlen;
    int pos;

    if (script[i] == 0) return -1;
    delim = script[i];
    i++;
    while (script[i] != 0 && script[i] != delim) {
        char c;
        if (pn + 1 >= LINE_MAX) return -1;
        if (!take_esc(script, &i, &c)) return -1;
        pat[pn++] = c;
    }
    if (script[i] != delim) return -1;
    i++;
    while (script[i] != 0 && script[i] != delim) {
        char c;
        if (rn + 1 >= LINE_MAX) return -1;
        if (!take_esc(script, &i, &c)) return -1;
        repl[rn++] = c;
    }
    if (script[i] != delim) return -1;
    i++;
    if (script[i] == 'g') {
        global = 1;
        i++;
    }
    *newoff = i;

    nlen = pn;
    rlen = rn;
    oi = 0;
    li = 0;
    left = *linelen;
    while (li <= left) {
        at = find_sub(line + li, left - li, pat, nlen);
        if (at < 0) {
            while (li < left) {
                if (oi + 1 >= OUT_MAX) return -1;
                out[oi++] = line[li++];
            }
            break;
        }
        pos = li + at;
        while (li < pos) {
            if (oi + 1 >= OUT_MAX) return -1;
            out[oi++] = line[li++];
        }
        for (at = 0; at < rlen; at++) {
            if (oi + 1 >= OUT_MAX) return -1;
            out[oi++] = repl[at];
        }
        li += nlen;
        if (nlen == 0) {
            if (li < left) {
                if (oi + 1 >= OUT_MAX) return -1;
                out[oi++] = line[li++];
            } else {
                break;
            }
        }
        if (!global) {
            while (li < left) {
                if (oi + 1 >= OUT_MAX) return -1;
                out[oi++] = line[li++];
            }
            break;
        }
    }
    if (oi >= LINE_MAX) return -1;
    for (at = 0; at < oi; at++) line[at] = out[at];
    line[oi] = 0;
    *linelen = oi;
    return 0;
}

static int run_script(char *line, int *linelen, const char *script, int *deleted) {
    int i = 0;
    *deleted = 0;
    while (script[i] != 0) {
        char c;
        int newoff;
        while (script[i] == ' ' || script[i] == '\t' || script[i] == '\n' ||
               script[i] == ';') {
            i++;
        }
        if (script[i] == 0) break;
        c = script[i];
        i++;
        if (c == 's') {
            newoff = i;
            if (do_subst(line, linelen, script, i, &newoff) != 0) return -1;
            i = newoff;
        } else if (c == 'd') {
            *deleted = 1;
            return 0;
        } else if (c == 'p') {
            if (*linelen > 0) u_write(1, line, (unsigned long)*linelen);
            u_write(1, "\n", 1);
        } else if (c == 'q') {
            quit_flag = 1;
        } else {
            return -1;
        }
        while (script[i] == ' ' || script[i] == '\t') i++;
        if (script[i] == ';') {
            i++;
            continue;
        }
        if (script[i] != 0 && script[i] != '\n') return -1;
    }
    return 0;
}

static int process_fd(long fd, const char *script) {
    char buf[256];
    char line[LINE_MAX];
    int llen = 0;
    long n;
    int i;
    int deleted;
    int had_partial = 0;

    quit_flag = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (buf[i] == '\n') {
                line[llen] = 0;
                deleted = 0;
                if (run_script(line, &llen, script, &deleted) != 0) {
                    u_write(2, "sed: bad script\n", 16);
                    return 1;
                }
                if (!deleted && !quiet) {
                    if (llen > 0) u_write(1, line, (unsigned long)llen);
                    u_write(1, "\n", 1);
                }
                llen = 0;
                had_partial = 0;
                if (quit_flag) return 0;
            } else {
                if (llen + 1 >= LINE_MAX) {
                    u_write(2, "sed: line too long\n", 19);
                    return 1;
                }
                line[llen++] = buf[i];
                had_partial = 1;
            }
        }
    }
    if (n < 0) return 1;
    if (had_partial || llen > 0) {
        line[llen] = 0;
        deleted = 0;
        if (run_script(line, &llen, script, &deleted) != 0) {
            u_write(2, "sed: bad script\n", 16);
            return 1;
        }
        if (!deleted && !quiet) {
            if (llen > 0) u_write(1, line, (unsigned long)llen);
            u_write(1, "\n", 1);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *script;
    int rc = 0;

    quiet = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-n") == 0) {
            quiet = 1;
            i++;
            continue;
        }
        break;
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    script = argv[i];
    i++;
    if (script[0] == 0) {
        usage();
        return 1;
    }

    if (i >= argc) {
        return process_fd(0, script);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            fd = 0;
        } else {
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("sed: %s: no such file\n", argv[i]);
                rc = 1;
                continue;
            }
        }
        if (process_fd(fd, script) != 0) rc = 1;
        if (fd != 0) u_close(fd);
        if (quit_flag) break;
    }
    return rc;
}
