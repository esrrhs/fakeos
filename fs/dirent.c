package fs;
import types;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* Linux getdents64 field layout. Keep 48-byte minimum padding for old short
 * names; d_reclen is authoritative. Long names use up to 280 bytes. */
u32 dirent_size(u32 nlen) {
    if (nlen == 0 || nlen > 255) return 0;
    u32 n = (19 + nlen + 1 + 7) & ~7u;
    return n < 48 ? 48 : n;
}

u32 dirent_write(char *out, u64 room, u64 ino, u64 next, u32 type,
                 const char *name, u32 nlen) {
    u32 n = dirent_size(nlen);
    u32 i;
    if (!n || room < n || !out || !name) return 0;
    for (i = 0; i < nlen; i++) {
        if (!name[i] || name[i] == '/') return 0;
    }
    for (i = 0; i < n; i++) out[i] = 0;
    for (i = 0; i < 8; i++) {
        out[i] = (char)(ino >> (8 * i));
        out[8 + i] = (char)(next >> (8 * i));
    }
    out[16] = (char)n;
    out[17] = (char)(n >> 8);
    out[18] = (char)type;
    for (i = 0; i < nlen; i++) out[19 + i] = name[i];
    return n;
}

u32 dirent_record_size(const char *p, u64 avail) {
    u32 n;
    u32 i;
    if (!p || avail < 24) return 0;
    n = (u32)(unsigned char)p[16] | ((u32)(unsigned char)p[17] << 8);
    if (n < 24 || (n & 7) || n > avail) return 0;
    for (i = 19; i < n && i < 275; i++) {
        if (!p[i]) return i > 19 ? n : 0;
        if (p[i] == '/') return 0;
    }
    return 0;
}
