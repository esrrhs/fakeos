/* fakeos userland - tsort: topological sort of ordered pairs (thin POSIX).
 * Forms: tsort [FILE...]
 * stdin if no FILE. Whitespace-separated tokens as predecessor/successor
 * pairs; an odd trailing token is a lone node. Detects cycles. Compact.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);

enum {
    MAX_NODES = 128,
    NAME_LEN = 64,
    MAX_EDGES = 512,
    TOK_LEN = 64
};

static char names[MAX_NODES][NAME_LEN];
static int nnodes;
static int edge_u[MAX_EDGES];
static int edge_v[MAX_EDGES];
static int nedges;
static int indeg[MAX_NODES];
static int seen_out[MAX_NODES];
static int queue[MAX_NODES];
static int emitted[MAX_NODES];

static int find_or_add(const char *s) {
    int i;
    unsigned long n;
    if (s == 0 || s[0] == 0) return -1;
    for (i = 0; i < nnodes; i++) {
        if (strcmp(names[i], s) == 0) return i;
    }
    if (nnodes >= MAX_NODES) {
        u_write(2, "tsort: too many nodes\n", 22);
        return -1;
    }
    n = strlen(s);
    if (n >= NAME_LEN) {
        u_write(2, "tsort: token too long\n", 22);
        return -1;
    }
    strcpy(names[nnodes], s);
    return nnodes++;
}

static int add_edge(int u, int v) {
    int i;
    if (u < 0 || v < 0) return 1;
    if (u == v) return 0; /* self-loop: ignore as ordering noop */
    for (i = 0; i < nedges; i++) {
        if (edge_u[i] == u && edge_v[i] == v) return 0;
    }
    if (nedges >= MAX_EDGES) {
        u_write(2, "tsort: too many edges\n", 22);
        return 1;
    }
    edge_u[nedges] = u;
    edge_v[nedges] = v;
    nedges++;
    return 0;
}

static int feed_token(const char *tok, int *have_prev, int *prev) {
    int id = find_or_add(tok);
    if (id < 0) return 1;
    if (!*have_prev) {
        *prev = id;
        *have_prev = 1;
        return 0;
    }
    if (add_edge(*prev, id) != 0) return 1;
    *have_prev = 0;
    *prev = -1;
    return 0;
}

static int ingest_fd(long fd, int *have_prev, int *prev) {
    char buf[256];
    char tok[TOK_LEN];
    long n;
    long i;
    int ti = 0;

    while ((n = u_read(fd, buf, 256)) > 0) {
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (ti > 0) {
                    tok[ti] = 0;
                    if (feed_token(tok, have_prev, prev) != 0) return 1;
                    ti = 0;
                }
            } else {
                if (ti + 1 >= TOK_LEN) {
                    u_write(2, "tsort: token too long\n", 22);
                    return 1;
                }
                tok[ti++] = c;
            }
        }
    }
    if (ti > 0) {
        tok[ti] = 0;
        if (feed_token(tok, have_prev, prev) != 0) return 1;
    }
    return n < 0 ? 1 : 0;
}

static int do_tsort(void) {
    int i;
    int qh;
    int qt;
    int nout;

    for (i = 0; i < nnodes; i++) {
        indeg[i] = 0;
        seen_out[i] = 0;
        emitted[i] = 0;
    }
    for (i = 0; i < nedges; i++) {
        indeg[edge_v[i]]++;
    }

    qh = 0;
    qt = 0;
    for (i = 0; i < nnodes; i++) {
        if (indeg[i] == 0) {
            queue[qt++] = i;
        }
    }

    nout = 0;
    while (qh < qt) {
        int u = queue[qh++];
        int e;
        emitted[u] = 1;
        u_write(1, names[u], strlen(names[u]));
        u_write(1, "\n", 1);
        nout++;
        for (e = 0; e < nedges; e++) {
            if (edge_u[e] != u) continue;
            indeg[edge_v[e]]--;
            if (indeg[edge_v[e]] == 0) {
                queue[qt++] = edge_v[e];
            }
        }
    }

    if (nout != nnodes) {
        u_write(2, "tsort: cycle detected\n", 22);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int have_prev = 0;
    int prev = -1;

    nnodes = 0;
    nedges = 0;

    if (i < argc && strcmp(argv[i], "--") == 0) i++;

    if (i >= argc) {
        if (ingest_fd(0, &have_prev, &prev) != 0) return 1;
    } else {
        for (; i < argc; i++) {
            long fd;
            if (strcmp(argv[i], "-") == 0) {
                if (ingest_fd(0, &have_prev, &prev) != 0) return 1;
                continue;
            }
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                u_write(2, "tsort: open failed\n", 19);
                return 1;
            }
            if (ingest_fd(fd, &have_prev, &prev) != 0) {
                u_close(fd);
                return 1;
            }
            u_close(fd);
        }
    }

    /* Odd trailing token is already a lone node via find_or_add. */
    (void)have_prev;
    (void)prev;
    (void)rc;
    if (nnodes == 0) return 0;
    return do_tsort();
}
