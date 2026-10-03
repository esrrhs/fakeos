/* ldfake - minimal in-OS static linker for the fakeos self-bootstrap
 * (milestone 5, task 6).
 *
 * Consumes ELF64 relocatable objects produced by fakecc (-c) and nasm
 * (-f elf64), resolves the relocations the fakeos kernel actually uses
 * (R_X86_64_64 / PC32 / PLT32 / GOTPCREL and the nasm-emitted
 * R_X86_64_32 / R_X86_64_32S), merges sections into the
 * low/high layout defined by boot/linker.ld, and emits a phdr-only
 * multiboot ELF32 image that QEMU boots directly.
 *
 * Not a general ELF linker: no shared objects, no dynamic linking, no
 * section headers in the output. Input set/order is fixed by /bin/kbuild.
 *
 * FakeOS user program: links against the minimal fakeos libc. */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long len);
extern long u_write(long fd, const void *buf, unsigned long len);
extern void u_exit(long code);

extern void printf(const char *fmt, ...);
extern void puterr(const char *s);
extern void *malloc(unsigned long size);
extern void free(void *ptr);
extern void memset(void *dst, int c, unsigned long n);
extern void memcpy(void *dst, const void *src, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);

enum {
    ELFCLASS64 = 2,
    ET_REL = 1,
    EM_X86_64 = 62,
    SHT_SYMTAB = 2,
    SHT_NOBITS = 8,
    SHT_RELA = 4,
    SHF_ALLOC = 2,
    R_64 = 1, R_PC32 = 2, R_PLT32 = 4, R_GOTPCREL = 9,
    R_32 = 10, R_32S = 11, R_GOTPCRELX = 41, R_PLT32X = 42,
    PT_LOAD = 1,
    O_RDWR_CREAT = 0x42,
    OBJ_ARENA = 786432,      /* all inputs total ~400 KiB */
    OUT_CAP = 2097152,
    KERNEL_LMA = 0x100000,
    KERNEL_VMA = 0xFFFFFFFF80000000UL,
    SHN_ABS_SPC = 0xfff1,
    HDR_PAD = 0x1000
};

static unsigned short rd16(const unsigned char *p) {
    return (unsigned short)(p[0] | (p[1] << 8));
}
static unsigned int rd32(const unsigned char *p) {
    return (unsigned int)(p[0] | (p[1] << 8) | (p[2] << 16) |
                         (p[3] << 24));
}
static unsigned long rd64(const unsigned char *p) {
    unsigned long v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}
static long rds64(const unsigned char *p) {
    unsigned long u = rd64(p);
    if (u >= 0x8000000000000000UL) {
        return (long)(u - 0x8000000000000000UL) - (long)0x8000000000000000UL;
    }
    return (long)u;
}
static void wr16(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void wr32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
static void wr64(unsigned char *p, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (i * 8));
    }
}

struct obj;

struct isec {
    struct obj *obj;
    unsigned int sh;      /* section index in owning object */
    unsigned long size;
    unsigned long align;
    unsigned long vma;     /* final load address (LMA) */
    unsigned long off;     /* final file offset; 0 for NOBITS */
    int nobits;
    int high;              /* 1: linked at KERNEL_VMA + LMA */
};

struct obj {
    unsigned char *data;
    unsigned long size;
    unsigned int shnum, shentsize;
    unsigned long shoff;
    struct isec secs[48];
    unsigned int nsec;
};

static struct obj objs[40];
static unsigned int n_objs;

/* Output buckets, in placement order (must match boot/linker.ld). */
enum {
    B_MULTIBOOT = 0, B_BOOTTEXT, B_BOOTDATA, B_BOOTBSS,
    B_TEXT, B_RODATA, B_GOT, B_DATA, B_BSS,
    NBUCKETS
};
static struct isec *buckets[NBUCKETS][80];
static unsigned int bcount[NBUCKETS];
static struct isec got_sec;      /* synthetic .got placed in B_GOT */

static unsigned long got_slots[256];   /* symbol VMA per GOT entry */

/* Placement results (filled by layout). text_lma/file_end/bss_end are
 * load addresses; boundary LMA vars feed the synthetic linker-script
 * symbols. */
static unsigned long text_lma, text_vma, low_end, file_end, bss_end;
static unsigned long b_text_lma, b_text_end_lma;
static unsigned long b_rodata_lma, b_rodata_end_lma;
static unsigned long b_data_lma, b_data_end_lma;
static unsigned long b_bss_lma;

static unsigned long align_up(unsigned long v, unsigned long a) {
    if (a <= 1) {
        return v;
    }
    return (v + a - 1) & ~(a - 1);
}

static int streq(const char *a, const char *b) {
    unsigned long i;
    if (strlen(a) != strlen(b)) {
        return 0;
    }
    for (i = 0; a[i]; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int bucket_for(const char *nm, unsigned long flags, int type) {
    (void)flags;
    if (type == SHT_NOBITS) {
        if (streq(nm, ".boot.bss")) return B_BOOTBSS;
        if (streq(nm, ".bss")) return B_BSS;
    }
    if (streq(nm, ".multiboot")) return B_MULTIBOOT;
    if (streq(nm, ".boot.text")) return B_BOOTTEXT;
    if (streq(nm, ".boot.data")) return B_BOOTDATA;
    if (streq(nm, ".text")) return B_TEXT;
    if (streq(nm, ".rodata")) return B_RODATA;
    if (streq(nm, ".data")) return B_DATA;
    if (streq(nm, ".bss")) return B_BSS;
    return -1;     /* .note.GNU-stack / COMMON / debug tables: ignored */
}

static const unsigned char *obj_sh(struct obj *o, unsigned int idx);
static unsigned long sym_global_vma(const char *name);
static unsigned long sym_vma(struct obj *o, unsigned int symidx,
                           const char **name_out);

static unsigned char obj_arena[OBJ_ARENA];
static unsigned long arena_used;
/* File scope (not function-static): fakecc emits zero-init file-scope
 * arrays into NOBITS .bss; a 2 MiB function-scope static would
 * otherwise land in PROGBITS and bloat the embedded ELF. */
static unsigned char outbuf[OUT_CAP];

static int read_obj(const char *path) {
    long fd = u_open(path, 0, 0);
    long r;
    unsigned long len = 0;
    unsigned char *buf;
    struct obj *o;
    unsigned int i;
    if (fd < 0) {
        printf("ldfake: cannot open %s\n", (long)path);
        return 0;
    }
    if (arena_used >= OBJ_ARENA) {
        puterr("ldfake: object arena full");
        return 0;
    }
    buf = obj_arena + arena_used;
    while (len < OBJ_ARENA - arena_used) {
        r = u_read(fd, buf + len, OBJ_ARENA - arena_used - len);
        if (r <= 0) {
            break;
        }
        len += (unsigned long)r;
    }
    u_close(fd);
    if (len < 64 || buf[0] != 0x7f || buf[1] != 'E' ||
        buf[2] != 'L' || buf[3] != 'F' || buf[4] != ELFCLASS64 ||
        rd16(buf + 16) != ET_REL || rd16(buf + 18) != EM_X86_64) {
        printf("ldfake: %s not x86-64 reloc ELF\n", (long)path);
        return 0;
    }
    arena_used += len;
    o = &objs[n_objs++];
    o->data = buf;
    o->size = len;
    o->shoff = rd64(buf + 40);
    o->shentsize = rd16(buf + 58);
    o->shnum = rd16(buf + 60);
    for (i = 0; i < o->shnum; i++) {
        const unsigned char *sh =
            buf + o->shoff + (unsigned long)i * o->shentsize;
        unsigned int type = rd32(sh + 4);
        unsigned long flags = rd64(sh + 8);
        unsigned long size = rd64(sh + 32);
        unsigned long align = rd64(sh + 48);
        const char *sname = (const char *)
            (buf + rd64(obj_sh(o, rd16(buf + 62)) + 24) + rd32(sh));
        int b = bucket_for(sname, flags, type);
        if (b < 0) {
            continue;
        }
        if (bcount[b] >= 80 || o->nsec >= 48) {
            puterr("ldfake: section tables full");
            return 0;
        }
        struct isec *s = &o->secs[o->nsec++];
        s->obj = o;
        s->sh = i;
        s->size = size;
        s->align = align ? align : 1;
        s->nobits = (type == SHT_NOBITS);
        buckets[b][bcount[b]++] = s;
    }
    return 1;
}

static struct isec *obj_section(struct obj *o, unsigned int idx) {
    unsigned int i;
    for (i = 0; i < o->nsec; i++) {
        if (o->secs[i].sh == idx) {
            return &o->secs[i];
        }
    }
    return 0;
}

static const unsigned char *obj_sh(struct obj *o, unsigned int idx) {
    return o->data + o->shoff + (unsigned long)idx * o->shentsize;
}

/* Linked virtual address of a placed section. Low boot sections keep
 * their identity LMA; high-half sections map to KERNEL_VMA + LMA
 * (linker script: "." advances from LMA by KERNEL_VMA), matching
 * boot/linker.ld and the kernel's "&sym - KERNEL_VMA" physical math. */
static unsigned long sec_link_vma(struct isec *s) {
    if (s->high) {
        return KERNEL_VMA + s->vma;
    }
    return s->vma;
}

/* Resolve one object-local symbol (obj, symtab row) to its final VMA.
 * Returns 0 for undefined symbols; *name_out points to its name. */
static unsigned long sym_vma(struct obj *o, unsigned int symidx,
                           const char **name_out) {
    unsigned int link;
    const unsigned char *strtab;
    const unsigned char *sym;
    unsigned short shndx;
    struct isec *s;
    /* find this object's symtab section by scanning sections (sh_type=2) */
    unsigned int i;
    const unsigned char *ss = 0;
    for (i = 0; i < o->shnum; i++) {
        if (rd32(obj_sh(o, i) + 4) == SHT_SYMTAB) {
            ss = obj_sh(o, i);
            break;
        }
    }
    if (!ss) {
        return 0;
    }
    link = rd32(ss + 40);
    strtab = o->data + rd64(obj_sh(o, link) + 24);
    sym = o->data + rd64(ss + 24) + (unsigned long)symidx * 24;
    if (name_out) {
        *name_out = (const char *)(strtab + rd32(sym));
    }
    shndx = rd16(sym + 6);
    if (shndx == SHN_ABS_SPC) {
        return rd64(sym + 8);          /* nasm EQU constant */
    }
    if (shndx == 0 || shndx >= o->shnum) {
        return 0;
    }
    s = obj_section(o, shndx);
    if (s == 0) {
        return 0;
    }
    return sec_link_vma(s) + rd64(sym + 8);
}

/* First pass: count GOT entries (distinct (obj,symidx) pairs in
 * GOTPCREL-family relocs). */
static char got_names[256][64];
static unsigned int got_count;

static int got_slot(const char *name) {
    unsigned int i;
    unsigned long k;
    for (i = 0; i < got_count; i++) {
        if (streq(got_names[i], name)) {
            return (int)i;
        }
    }
    if (got_count >= 256) {
        return -1;
    }
    for (k = 0; k < 63 && name[k]; k++) {
        got_names[got_count][k] = name[k];
    }
    got_names[got_count][k] = 0;
    return (int)got_count++;
}

/* GOT identity key: globals by name (shared across objects), locals
 * per (object index, symbol row). Writes into dst (64 bytes). */
static void got_key_for(struct obj *o, unsigned int oi, unsigned int link,
                        unsigned int symi, char *dst) {
    /* 'link' is the RELA section's sh_link: index of the symtab. The
     * strtab is one more indirection through the symtab's sh_link. */
    unsigned int strlink = rd32(obj_sh(o, link) + 40);
    const unsigned char *strtab =
        o->data + rd64(obj_sh(o, strlink) + 24);
    const unsigned char *sym =
        o->data + rd64(obj_sh(o, link) + 24) + (unsigned long)symi * 24;
    const char *nm = (const char *)(strtab + rd32(sym));
    unsigned short shndx = rd16(sym + 6);
    unsigned char bind = sym[4] >> 4;
    unsigned long k = 0;
    if (shndx == 0 || bind == 1) {
        dst[k++] = 'g'; dst[k++] = ':';
        while (nm[k - 2] && k < 62) {
            dst[k] = nm[k - 2];
            k++;
        }
    } else {
        char tmp[10];
        int n = 0;
        unsigned long v;
        dst[k++] = 'l'; dst[k++] = ':';
        v = oi;
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) { dst[k++] = tmp[--n]; }
        dst[k++] = ':';
        n = 0;
        v = symi;
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) { dst[k++] = tmp[--n]; }
    }
    dst[k] = 0;
}

/* Linker-script-provided symbols (boot/linker.ld); no object defines
 * them. Values are high-half VMAs of the placed bucket boundaries. */
static unsigned long synth_vma(const char *name) {
    if (streq(name, "_kernel_text_start")) {
        return KERNEL_VMA + b_text_lma;
    }
    if (streq(name, "_kernel_text_end")) {
        return KERNEL_VMA + b_text_end_lma;
    }
    if (streq(name, "_kernel_rodata_start")) {
        return KERNEL_VMA + b_rodata_lma;
    }
    if (streq(name, "_kernel_rodata_end")) {
        return KERNEL_VMA + b_rodata_end_lma;
    }
    if (streq(name, "_kernel_data_start")) {
        return KERNEL_VMA + b_data_lma;
    }
    if (streq(name, "_kernel_data_end")) {
        return KERNEL_VMA + b_data_end_lma;
    }
    if (streq(name, "_kernel_bss_start")) {
        return KERNEL_VMA + b_bss_lma;
    }
    if (streq(name, "_kernel_bss_end") || streq(name, "_kernel_end")) {
        return KERNEL_VMA + bss_end;
    }
    return 0;
}

/* Resolve a relocation's symbol to its VMA. Defined symbols use
 * their own object's section; SHN_UNDEF globals (cross-object
 * references) resolve by name across every object, falling back to
 * the linker-script synthetic symbols. */
static unsigned long reloc_sym_vma(struct obj *o, unsigned int link,
                                  unsigned int symi, const char **nm_out) {
    unsigned int strlink = rd32(obj_sh(o, link) + 40);
    const unsigned char *strtab =
        o->data + rd64(obj_sh(o, strlink) + 24);
    const unsigned char *sym =
        o->data + rd64(obj_sh(o, link) + 24) + (unsigned long)symi * 24;
    const char *nm = (const char *)(strtab + rd32(sym));
    unsigned short shndx = rd16(sym + 6);
    unsigned long v;
    if (nm_out) {
        *nm_out = nm;
    }
    if (shndx == SHN_ABS_SPC) {
        return rd64(sym + 8);
    }
    if (shndx == 0) {
        v = sym_global_vma(nm);
        if (v) {
            return v;
        }
        return synth_vma(nm);
    }
    return sym_vma(o, symi, 0);
}

static int scan_got(void) {
    unsigned int i;
    for (i = 0; i < n_objs; i++) {
        struct obj *o = &objs[i];
        unsigned int si;
        for (si = 0; si < o->shnum; si++) {
            const unsigned char *sh = obj_sh(o, si);
            unsigned int type = rd32(sh + 4);
            unsigned int link = rd32(sh + 40);
            unsigned long entsize, cnt, k;
            if (type != SHT_RELA || link >= o->shnum) {
                continue;
            }
            entsize = rd64(sh + 56);
            if (entsize == 0) {
                entsize = 24;   /* Elf64_Rela (tolerate entsize=0) */
            }
            cnt = rd64(sh + 32) / entsize;
            for (k = 0; k < cnt; k++) {
                const unsigned char *r =
                    o->data + rd64(sh + 24) + k * entsize;
                unsigned int t = (unsigned int)rd32(r + 8);
                if (t == R_GOTPCREL || t == R_GOTPCRELX) {
                    unsigned int symi = (unsigned int)rd32(r + 12);
                    char key[64];
                    got_key_for(o, i, link, symi, key);
                    if (got_slot(key) < 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

/* Place every bucketed section, low region first, high kernel region next. */
static void layout(void) {
    int b;
    unsigned long cur = KERNEL_LMA;
    for (b = B_MULTIBOOT; b <= B_BOOTBSS; b++) {
        unsigned int i;
        for (i = 0; i < bcount[b]; i++) {
            struct isec *s = buckets[b][i];
            cur = align_up(cur, s->align);
            s->vma = cur;
            cur += s->size;
        }
    }
    low_end = cur;
    cur = align_up(low_end, 4096);
    text_lma = cur;
    text_vma = KERNEL_VMA + cur;
    b_text_lma = cur;
    for (b = B_TEXT; b <= B_DATA; b++) {
        unsigned long ba = (b == B_TEXT) ? 1 : 4096;
        unsigned int i;
        cur = align_up(cur, ba);
        if (b == B_RODATA) { b_rodata_lma = cur; }
        if (b == B_GOT)    { (void)0; }     /* GOT has no boundary symbols */
        if (b == B_DATA)  { b_data_lma = cur; }
        for (i = 0; i < bcount[b]; i++) {
            struct isec *s = buckets[b][i];
            cur = align_up(cur, s->align);
            s->vma = cur;
            s->high = 1;
            cur += s->size;
        }
        if (b == B_TEXT)   { b_text_end_lma = cur; }
        if (b == B_RODATA) { b_rodata_end_lma = cur; }
        if (b == B_DATA)   { b_data_end_lma = cur; }
    }
    cur = align_up(cur, 4096);
    b_bss_lma = cur;
    {
        unsigned int i;
        for (i = 0; i < bcount[B_BSS]; i++) {
            struct isec *s = buckets[B_BSS][i];
            cur = align_up(cur, s->align);
            s->vma = cur;
            s->high = 1;
            cur += s->size;
        }
    }
    bss_end = cur;
    file_end = 0;
    for (b = B_MULTIBOOT; b <= B_DATA; b++) {
        unsigned int i;
        for (i = 0; i < bcount[b]; i++) {
            struct isec *s = buckets[b][i];
            if (!s->nobits) {
                s->off = s->vma - KERNEL_LMA + HDR_PAD;
            }
            unsigned long e = s->off + s->size;
            if (!s->nobits && e > file_end) {
                file_end = e;
            }
        }
    }
}

static int apply_relocs(unsigned char *out) {
    unsigned int i;
    int fail = 0;
    for (i = 0; i < n_objs && !fail; i++) {
        struct obj *o = &objs[i];
        unsigned int si;
        for (si = 0; si < o->shnum; si++) {
            const unsigned char *sh = obj_sh(o, si);
            unsigned int type = rd32(sh + 4);
            unsigned int link = rd32(sh + 40);
            unsigned int target = rd32(sh + 44);
            unsigned long entsize, cnt, k;
            struct isec *ts;
            if (type != SHT_RELA || link >= o->shnum) {
                continue;
            }
            entsize = rd64(sh + 56);
            if (entsize == 0) {
                entsize = 24;   /* Elf64_Rela (tolerate entsize=0) */
            }
            ts = obj_section(o, target);
            if (ts == 0) {
                continue;   /* reloc against a discarded section */
            }
            if (ts->nobits) {
                continue;   /* BSS has no file bytes to patch */
            }
            cnt = rd64(sh + 32) / entsize;
            for (k = 0; k < cnt; k++) {
                const unsigned char *r =
                    o->data + rd64(sh + 24) + k * entsize;
                unsigned long roff = rd64(r);
                unsigned int rt2 = (unsigned int)rd32(r + 8);
                unsigned int symi = (unsigned int)rd32(r + 12);
                long addend = rds64(r + 16);
                unsigned long p = sec_link_vma(ts) + roff;
                if (rt2 == 0) {
                    continue;           /* R_X86_64_NONE placeholder */
                }
                unsigned char *patch = out + ts->off + roff;
                if (rt2 == R_64) {
                    const char *nm = 0;
                    unsigned long s = reloc_sym_vma(o, link, symi, &nm);
                    if (!s) {
                        printf("ldfake: reloc %u against undefined '%s'\n",
                                    (unsigned long)rt2, (long)nm);
                        fail = 1;
                        break;
                    }
                    wr64(patch, s + (unsigned long)addend);
                } else if (rt2 == R_PC32 || rt2 == R_PLT32 ||
                           rt2 == R_PLT32X) {
                    const char *nm = 0;
                    unsigned long s = reloc_sym_vma(o, link, symi, &nm);
                    if (!s) {
                        printf("ldfake: reloc %u against undefined '%s'\n",
                                    (unsigned long)rt2, (long)nm);
                        fail = 1;
                        break;
                    }
                    wr32(patch, (unsigned long)((long)s + addend - (long)p));
                } else if (rt2 == R_GOTPCREL || rt2 == R_GOTPCRELX) {
                    char gkey[64];
                    int g;
                    unsigned long gaddr;
                    got_key_for(o, i, link, symi, gkey);
                    g = got_slot(gkey);
                    if (g < 0) {
                        printf("ldfake: GOT missing '%s'\n", (long)gkey);
                        fail = 1;
                        break;
                    }
                    /* R_X86_64_GOTPCREL: displacement to the GOT ENTRY
                     * (G + A - P); the entry itself holds symbol VMA. */
                    gaddr = sec_link_vma(&got_sec) + (unsigned long)g * 8;
                    wr32(patch, (unsigned long)
                            ((long)gaddr + addend - (long)p));
                } else if (rt2 == R_32 || rt2 == R_32S) {
                    const char *nm = 0;
                    unsigned long s = reloc_sym_vma(o, link, symi, &nm);
                    if (!s) {
                        printf("ldfake: reloc %u against undefined '%s'\n",
                                    (unsigned long)rt2, (long)nm);
                        fail = 1;
                        break;
                    }
                    wr32(patch, s + (unsigned long)addend);
                } else {
                    printf("ldfake: unsupported reloc %u\n", (unsigned long)rt2);
                    fail = 1;
                    break;
                }
            }
        }
    }
    return !fail;
}

static unsigned long sym_global_vma(const char *name) {
    unsigned int i;
    for (i = 0; i < n_objs; i++) {
        struct obj *o = &objs[i];
        unsigned int si;
        for (si = 0; si < o->shnum; si++) {
            const unsigned char *sh = obj_sh(o, si);
            unsigned int link, cnt, k;
            const unsigned char *strtab;
            if (rd32(sh + 4) != SHT_SYMTAB) {
                continue;
            }
            link = rd32(sh + 40);
            strtab = o->data + rd64(obj_sh(o, link) + 24);
            cnt = rd64(sh + 32) / 24;
            for (k = 0; k < cnt; k++) {
                const unsigned char *sym =
                    o->data + rd64(sh + 24) + k * 24;
                unsigned char bind = sym[4] >> 4;
                const char *nm2 = (const char *)(strtab + rd32(sym));
                unsigned short shndx = rd16(sym + 6);
                struct isec *s;
                if (bind == 1 && shndx != 0 &&
                    shndx != SHN_ABS_SPC && streq(nm2, name)) {
                    s = obj_section(o, shndx);
                    if (s) {
                        return sec_link_vma(s) + rd64(sym + 8);
                    }
                }
                if (bind == 1 && shndx == SHN_ABS_SPC &&
                    streq(nm2, name)) {
                    return rd64(sym + 8);
                }
            }
        }
    }
    return 0;
}

/* Read an @response file: whitespace-separated object paths, appended
 * as inputs. (Execve supports at most 8 argv slots; the kernel link
 * list is ~35 files long.) */
static int read_atfile(const char *path) {
    long fd = u_open(path, 0, 0);
    long r;
    char buf[2048];
    unsigned long len = 0;
    if (fd < 0) {
        printf("ldfake: cannot open @%s\n", (long)path);
        return 0;
    }
    while (len < 2048) {
        r = u_read(fd, buf + len, 2048 - len);
        if (r <= 0) {
            break;
        }
        len += (unsigned long)r;
    }
    u_close(fd);
    {
        unsigned long i = 0;
        while (i < len) {
            char tok[128];
            unsigned long n = 0;
            while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                                  buf[i] == '\n' || buf[i] == '\r')) {
                i++;
            }
            while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
                       buf[i] != '\n' && buf[i] != '\r' && n < 127) {
                tok[n++] = buf[i++];
            }
            if (n) {
                tok[n] = 0;
                if (!read_obj(tok)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *outpath = "a.out";
    unsigned char *out;
    unsigned long mem_lma, entry;
    long fd;
    unsigned long written;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] == '@') {
            if (!read_atfile(argv[i] + 1)) {
                return 1;
            }
        } else if (!read_obj(argv[i])) {
            return 1;
        }
    }
    if (n_objs == 0) {
        puterr("ldfake: no inputs");
        return 1;
    }
    if (!scan_got()) {
        return 1;
    }
    memset(&got_sec, 0, 48);
    if (got_count) {
        got_sec.size = got_count * 8;
        got_sec.align = 8;
        buckets[B_GOT][bcount[B_GOT]++] = &got_sec;
    }
    layout();

    /* Fill GOT slots with the final symbol VMAs. */
    for (i = 0; i < (int)got_count; i++) {
        unsigned long v;
        if (got_names[i][0] == 'g') {
            v = sym_global_vma(got_names[i] + 2);
            if (!v) {
                v = synth_vma(got_names[i] + 2);
            }
        } else {
            /* l:<objidx>:<symrow> local symbol */
            const char *p = got_names[i] + 2;
            unsigned int oi = 0;
            unsigned int sr = 0;
            while (*p != ':') {
                oi = oi * 10 + (unsigned int)(*p - '0');
                p++;
            }
            p++;
            while (*p) {
                sr = sr * 10 + (unsigned int)(*p - '0');
                p++;
            }
            v = sym_vma(&objs[oi], sr, 0);
        }
        if (!v) {
            printf("ldfake: GOT symbol '%s' undefined\n",
                        (long)got_names[i]);
            return 1;
        }
        got_slots[i] = v;
    }

    entry = sym_global_vma("_start");
    if (entry == 0) {
        puterr("ldfake: no _start symbol");
        return 1;
    }
    if (entry > 0xFFFFFFFFUL) {
        printf("ldfake: entry %p outside low 4 GiB\n", entry);
        return 1;
    }

    out = outbuf;
    memset(out, 0, OUT_CAP);

    /* Copy PROGBITS input data into place. */
    for (i = 0; i < (int)n_objs; i++) {
        struct obj *o = &objs[i];
        unsigned int si;
        for (si = 0; si < o->nsec; si++) {
            struct isec *s = &o->secs[si];
            unsigned long soff;
            if (s->nobits || s->off == 0) {
                continue;
            }
            soff = rd64(obj_sh(o, s->sh) + 24);
            memcpy(out + s->off, o->data + soff, s->size);
        }
    }

    /* Fill GOT contents after copying (section input never produces GOT). */
    if (got_count) {
        unsigned long gbase = got_sec.off;
        for (i = 0; i < (int)got_count; i++) {
            wr64(out + gbase + (unsigned long)i * 8, got_slots[i]);
        }
    }

    if (!apply_relocs(out)) {
        return 1;
    }

    mem_lma = bss_end - text_lma;    /* high segment span incl. BSS */

    /* --- ELF32 header + two program headers at file offset 0 --- */
    out[0] = 0x7f;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 1;                     /* ELFCLASS32 */
    out[5] = 1;                     /* little endian */
    out[6] = 1;                     /* ELF version */
    wr16(out + 16, 2);             /* ET_EXEC */
    wr16(out + 18, 3);             /* EM_386 */
    wr32(out + 20, 1);             /* e_version */
    wr32(out + 24, (unsigned int)entry);
    wr32(out + 28, 52);            /* e_phoff (ehdr 52 bytes, phdrs before 0x1000 pad) */
    wr32(out + 32, 0);             /* e_shoff: none */
    wr32(out + 36, 0);             /* e_flags */
    wr16(out + 40, 52);            /* e_ehsize */
    wr16(out + 42, 32);            /* e_phentsize */
    wr16(out + 44, 2);             /* e_phnum */
    wr16(out + 46, 0);             /* e_shentsize */
    wr16(out + 48, 0);             /* e_shnum */
    wr16(out + 50, 0);             /* e_shstrndx */

    /* PH1: low 32-bit boot segment */
    {
        unsigned char *p = out + 52;
        wr32(p + 0, PT_LOAD);
        wr32(p + 4, HDR_PAD);
        wr32(p + 8, KERNEL_LMA);
        wr32(p + 12, KERNEL_LMA);
        wr32(p + 16, (unsigned int)(low_end - KERNEL_LMA));
        wr32(p + 20, (unsigned int)(low_end - KERNEL_LMA));
        wr32(p + 24, 6);             /* RW (matches GNU objcopy baseline) */
        wr32(p + 28, 0x1000);
    }
    /* PH2: high 64-bit kernel, truncated VMA (matches GNU objcopy) */
    {
        unsigned char *p = out + 52 + 32;
        unsigned long high_off = text_lma - KERNEL_LMA + HDR_PAD;
        unsigned long data_end = file_end - HDR_PAD;
        wr32(p + 0, PT_LOAD);
        wr32(p + 4, (unsigned int)high_off);
        wr32(p + 8, (unsigned int)(text_vma & 0xFFFFFFFFUL));
        wr32(p + 12, text_lma);
        wr32(p + 16, (unsigned int)(data_end - (text_lma - KERNEL_LMA)));
        wr32(p + 20, (unsigned int)mem_lma);
        wr32(p + 24, 7);             /* RWX (matches GNU objcopy baseline) */
        wr32(p + 28, 0x10);
    }

    fd = u_open(outpath, O_RDWR_CREAT, 0x1FF);
    if (fd < 0) {
        printf("ldfake: cannot create %s\n", (long)outpath);
        return 1;
    }
    written = u_write(fd, out, file_end);
    u_close(fd);
    if (written != file_end) {
        puterr("ldfake: short write");
        return 1;
    }
    printf("[ldfake] wrote %s: entry %p text %p file %u bytes, "
                "mem end %p\n",
                (long)outpath, entry, text_vma, file_end,
                KERNEL_VMA + bss_end);
    return 0;
}
