/* fakeos userland - sed: thin stream editor (s///g + d/p/q).
 * Forms: sed [-n] [-i] [-e SCRIPT]... [SCRIPT] [FILE...]
 * SCRIPT: semicolon-separated commands (no addresses):
 *   s/PAT/REPL/[gi] fixed-string substitute (g=all; i/I=ignore case)
 *   d               delete line (suppress default print)
 *   p               print pattern space
 *   q               quit after this line
 * Delimiter after s may be any non-NUL (e.g. s#a#b#). PAT/REPL: literals plus
 * \\ \n \t escapes. Flags after closing delim: g, i/I (any order, e.g. gi).
 * -e SCRIPT appends an expression (joined with ';'; glued -eSCRIPT). With any
 * -e, there is no positional SCRIPT operand (POSIX). Cap MAX_EXPR for
 * EXEC_MAX_ARGC=8. -n suppresses default print. -i edits FILE in place via a
 * same-dir temp then rename(2) (no backup ext; refuses stdin/-). stdin if no
 * FILE (without -i); "-" stdin. No regex, no addresses, no hold space.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    LINE_MAX = 512,
    OUT_MAX = 1024,
    PATH_CAP = 128,
    SCRIPT_CAP = 512,
    MAX_EXPR = 4,
    O_WRONLY = 1,
    O_CREAT = 0x40,
    O_TRUNC = 0x200,
    MODE_0666 = 0x1B6
};

static int quiet; /* -n */
static int inplace; /* -i */
static int quit_flag;
static long out_fd = 1;
static char joined[SCRIPT_CAP];
static int join_len;
static int n_expr;

static void usage(void) {
    u_write(2, "usage: sed [-ni] [-e SCRIPT]... [SCRIPT] [FILE...]\n", 52);
}

static int append_expr(const char *s) {
    unsigned long n;
    unsigned long i;
    if (s == 0 || s[0] == 0) return 0;
    if (n_expr >= MAX_EXPR) {
        u_write(2, "sed: too many -e\n", 17);
        return 0;
    }
    n = strlen(s);
    if (join_len > 0) {
        if ((unsigned long)join_len + 1 + n >= SCRIPT_CAP) {
            u_write(2, "sed: script too long\n", 21);
            return 0;
        }
        joined[join_len++] = ';';
    } else if (n >= SCRIPT_CAP) {
        u_write(2, "sed: script too long\n", 21);
        return 0;
    }
    if ((unsigned long)join_len + n >= SCRIPT_CAP) {
        u_write(2, "sed: script too long\n", 21);
        return 0;
    }
    for (i = 0; i < n; i++) joined[join_len++] = s[i];
    joined[join_len] = 0;
    n_expr++;
    return 1;
}

static void emit_out(const char *buf, unsigned long n) {
    if (n > 0) u_write(out_fd, buf, n);
}

static void emit_nl(void) {
    u_write(out_fd, "\n", 1);
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

static char fold_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* First index of needle[0..nlen) in hay[0..hlen), or -1. fold=1 ignores case. */
static int find_sub(const char *hay, int hlen, const char *needle, int nlen,
                    int fold) {
    int i;
    int j;
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (i = 0; i + nlen <= hlen; i++) {
        int ok = 1;
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (fold) {
                if (fold_c(a) != fold_c(b)) {
                    ok = 0;
                    break;
                }
            } else if (a != b) {
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
    int ign = 0;
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
    while (script[i] == 'g' || script[i] == 'i' || script[i] == 'I') {
        if (script[i] == 'g') global = 1;
        else ign = 1;
        i++;
    }
    *newoff = i;

    nlen = pn;
    rlen = rn;
    oi = 0;
    li = 0;
    left = *linelen;
    while (li <= left) {
        at = find_sub(line + li, left - li, pat, nlen, ign);
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
            if (*linelen > 0) emit_out(line, (unsigned long)*linelen);
            emit_nl();
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
                    if (llen > 0) emit_out(line, (unsigned long)llen);
                    emit_nl();
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
            if (llen > 0) emit_out(line, (unsigned long)llen);
            emit_nl();
        }
    }
    return 0;
}

/* Build same-dir temp path: PATH + ".sedtmp". */
static int make_tmp(char *dst, unsigned long cap, const char *path) {
    unsigned long i = 0;
    unsigned long j = 0;
    const char *suf = ".sedtmp";
    while (path[i] != 0 && i + 1 < cap) {
        dst[i] = path[i];
        i++;
    }
    while (suf[j] != 0 && i + 1 < cap) {
        dst[i++] = suf[j++];
    }
    if (suf[j] != 0) {
        dst[0] = 0;
        return 0;
    }
    dst[i] = 0;
    return 1;
}

static int process_inplace(const char *path, const char *script) {
    char tmp[PATH_CAP];
    long in_fd;
    long tmp_fd;
    int rc;

    if (!make_tmp(tmp, PATH_CAP, path)) {
        u_write(2, "sed: path too long for -i\n", 26);
        return 1;
    }
    in_fd = u_open(path, 0, 0);
    if (in_fd < 0) {
        printf("sed: %s: no such file\n", path);
        return 1;
    }
    tmp_fd = u_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, MODE_0666);
    if (tmp_fd < 0) {
        u_close(in_fd);
        u_write(2, "sed: cannot create temp\n", 24);
        return 1;
    }
    out_fd = tmp_fd;
    rc = process_fd(in_fd, script);
    out_fd = 1;
    u_close(in_fd);
    u_close(tmp_fd);
    if (rc != 0) {
        u_unlink(tmp);
        return 1;
    }
    if (u_rename(tmp, path) != 0) {
        u_unlink(tmp);
        printf("sed: %s: rename failed\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *script;
    int rc = 0;

    quiet = 0;
    inplace = 0;
    out_fd = 1;
    join_len = 0;
    n_expr = 0;
    joined[0] = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc || !append_expr(argv[i + 1])) {
                usage();
                return 1;
            }
            i += 2;
            continue;
        }
        if (argv[i][1] == 'e' && argv[i][2] != 0) {
            if (!append_expr(argv[i] + 2)) {
                usage();
                return 1;
            }
            i++;
            continue;
        }
        for (j = 1; argv[i][j]; j++) {
            if (argv[i][j] == 'n') {
                quiet = 1;
            } else if (argv[i][j] == 'i') {
                inplace = 1;
            } else if (argv[i][j] == 'e') {
                if (argv[i][j + 1] != 0) {
                    if (!append_expr(argv[i] + j + 1)) {
                        usage();
                        return 1;
                    }
                    j = 0;
                    break; /* consumed rest of this argv */
                }
                if (i + 1 >= argc || !append_expr(argv[i + 1])) {
                    usage();
                    return 1;
                }
                i += 2;
                goto next_arg;
            } else {
                usage();
                return 1;
            }
        }
        i++;
    next_arg:
        ;
    }

    if (n_expr > 0) {
        script = joined;
    } else {
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
    }

    if (i >= argc) {
        if (inplace) {
            u_write(2, "sed: -i requires a FILE\n", 24);
            return 1;
        }
        return process_fd(0, script);
    }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (inplace) {
                u_write(2, "sed: -i cannot edit stdin\n", 26);
                rc = 1;
                continue;
            }
            if (process_fd(0, script) != 0) rc = 1;
        } else if (inplace) {
            if (process_inplace(argv[i], script) != 0) rc = 1;
        } else {
            long fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("sed: %s: no such file\n", argv[i]);
                rc = 1;
                continue;
            }
            if (process_fd(fd, script) != 0) rc = 1;
            u_close(fd);
        }
        if (quit_flag) break;
    }
    return rc;
}
