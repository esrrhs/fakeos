#!/usr/bin/env bash
# ==============================================================================
# Milestone 5 toolchain: build a native x86_64-linux fakecc for embedding into
# fakeos' Ramfs. The fakecc source tree is NEVER modified: all work happens in
# a throwaway /tmp scratch copy.
#
# Why the scratch shims exist (WIP multi-target fakecc trees only):
#   - A WIP tree's v0/ self-hosting snapshot lags src/ (missing
#     cg64/target/a64 modules); running translate.py regenerates the dialect
#     sources, but macho.c (macOS backend) needs sys/wait.h and crashes the
#     bootstrap compiler when translated. The Linux stage1 does not need the
#     Mach-O backend, so that one module is dropped and its entry points are
#     provided by runtime/hoststub.c stubs.
#   - Output must be STATIC (fakeos has no ld.so); undefined symbols left by
#     the omitted macho module are fork/waitpid/execl/_exit, stubbed likewise.
#
# Upstream master fakecc is a single x86-64 backend (no macho/target/a64
# modules); this script auto-detects both layouts and also works with GNU
# sed (CI Ubuntu) and BSD sed (macOS).
#
# Usage: m5_build_fakecc.sh <output_path>
# Reads FAKECC_SRC (default: ../fakecc relative to the fakeos repo root).
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FAKECC_SRC=${FAKECC_SRC:-"$ROOT/../fakecc"}
OUT=$1

[ -d "$FAKECC_SRC/src" ] || { echo "FAKECC_SRC not found: $FAKECC_SRC" >&2; exit 1; }

# OUT must be absolute: this script cd's into the /tmp scratch tree below.
case "$OUT" in
    /*) ;;
    *)  OUT="$PWD/$OUT" ;;
esac

SCRATCH=/tmp/fakeos-m5-fakecc
STAMP="$SCRATCH/.translated"

# --- 1. fresh scratch copy ----------------------------------------------------
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
cp -R "$FAKECC_SRC/src" "$FAKECC_SRC/runtime" "$FAKECC_SRC/include" "$FAKECC_SRC/v0" "$SCRATCH/"

cd "$SCRATCH"

# macho.c only exists on WIP multi-target trees; everything Mach-O related
# below is gated on this.
HAS_MACHO=0
[ -f "$FAKECC_SRC/src/macho.c" ] && HAS_MACHO=1

# --- 2. host-side shim headers for the one-time gcc -E translation -----------
# WIP-only: macho.c's #include <sys/wait.h>/<unistd.h> have no dialect
# equivalents; provide minimal declarations so cpp translation succeeds.
if [ "$HAS_MACHO" = 1 ]; then
    mkdir -p v0/fakeinc/sys
    cat > v0/fakeinc/sys/wait.h <<'EOF'
typedef int pid_t;
int waitpid(pid_t pid, int *status, int options);
#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
EOF
    cat > v0/fakeinc/unistd.h <<'EOF'
typedef int pid_t;
pid_t fork(void);
int execl(const char *path, ...);
void _exit(int code);
EOF
fi

# Stage 0 must be a faithful host build of the compiler: the self-hosted
# /opt/homebrew/bin/fakecc is a WIP snapshot whose x86 backend miscompiles
# parts of the compiler (observed: compute_mcs_order builds a broken graph).
# fakecc's own CI builds stage 0 with clang/cmake (-O0); do the same entirely
# inside scratch so the fakecc source tree is never touched.
S0="$SCRATCH/s0host/fakecc"
if [ ! -x "$S0" ]; then
    echo "=== building clang stage 0 into scratch ==="
    CC=clang
    command -v clang >/dev/null 2>&1 || CC=cc
    cmake -S "$FAKECC_SRC" -B "$SCRATCH/s0host" \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS="-O0" >/dev/null
    cmake --build "$SCRATCH/s0host" --parallel >/dev/null
fi
[ -x "$S0" ] || { echo "stage 0 clang build failed" >&2; exit 1; }

# Translate src/*.c -> v0/*.c (the C preprocessor is clang here).
FAKECC="$S0" bash v0/build_bootstrap.sh >/dev/null 2>&1 || true
touch "$STAMP"

# --- 3. WIP-only: drop the Mach-O backend (crashes stage0 translation) -------
if [ "$HAS_MACHO" = 1 ]; then
    rm -f v0/macho.c
fi

# Portable in-place sed: BSD sed (macOS) needs `-i ''`, GNU sed wants `-i`.
# `-i.bak` is accepted by both; strip the backups afterwards.
if [ "$HAS_MACHO" = 1 ]; then
    sed -i.bak '/#pragma pack/d' v0/*.c 2>/dev/null || true
    rm -f v0/*.bak
fi

# WIP-only: translation on an Apple Silicon host bakes the arm64-macos
# default into target.c (compile-time __APPLE__/__aarch64__ selection).
# This artifact is a linux x86-64 executable that runs ON linux x86-64
# (fakeos), so flip host_default. On Linux-translated trees (and master,
# which has no target.c) there is nothing to do.
if [ -f v0/target.c ] && grep -q 'return &TARGET_ARM64_MACOS;' v0/target.c; then
    python3 - <<'PYEOF'
import re
p = "v0/target.c"
s = open(p).read()
s2, n = re.subn(r'(static const TargetDesc \*host_default\(void\) \{\s*)return &TARGET_ARM64_MACOS;',
                r'\1return &TARGET_X86_64_LINUX;', s)
assert n == 1, f"host_default patch matched {n} times"
open(p, "w").write(s2)
PYEOF
fi

# WIP-only stubs for symbols the removed macho.c used to provide.
#
# These MUST match macho.c's real signatures AND cover every non-static
# definition it exports. A gap here is silent and nasty: the undefined symbol
# becomes a PLT entry, which flips link.c's need_dynamic on and yields a
# PT_INTERP binary that cannot exec on fakeos (no ld.so). Step 5 re-checks
# this from the artifact itself, so a missed symbol fails the build loudly.
if [ "$HAS_MACHO" = 1 ]; then
cat > runtime/hoststub.c <<'EOF'
/* Scratch-only stubs: the Mach-O backend is omitted from the linux stage1.
 * Every symbol below is unreachable when the target is x86_64-linux; they
 * exist purely so the link resolves statically. Signatures mirror
 * src/macho.c and must stay in lock-step with it. */
package runtime;

struct EmitModule;
unsigned long macho_text_offset(void) { return 0; }
void macho_section_offsets(const struct EmitModule *em, unsigned long text_len,
                           unsigned long *ro_out, unsigned long *data_out,
                           unsigned long *bss_out) {
    (void)em; (void)text_len;
    if (ro_out) *ro_out = 0;
    if (data_out) *data_out = 0;
    if (bss_out) *bss_out = 0;
}
int macho_write_exec_text(const struct Buffer *text, unsigned long entry_off,
                          const char *path) {
    (void)text; (void)entry_off; (void)path; return -1;
}
int macho_write_exec(const struct EmitModule *em, unsigned long entry_off,
                     const char *path) {
    (void)em; (void)entry_off; (void)path; return -1;
}
int macho_write_object(const struct EmitModule *em, const char *path) {
    (void)em; (void)path; return -1;
}
int macho_read_object(const char *path, struct EmitModule *em) {
    (void)path; (void)em; return -1;
}
int macho_link_objects(struct EmitModule **mods, unsigned long n,
                       const char *path) {
    (void)mods; (void)n; (void)path; return -1;
}
int macho_codesign(const char *path) { (void)path; return -1; }
int fork(void) { return -1; }
int waitpid(int pid, int *status, int options) {
    (void)pid; (void)status; (void)options; return -1;
}
void _exit(int code) { __syscall(60, (long)code); }
int execl(const char *path, ...) { (void)path; return -1; }
EOF
fi

# --- 4. compile + self-link as a static linux x86-64 ELF ---------------------
# Module set is derived from src/*.c (same rule as upstream
# build_bootstrap.sh): WIP trees have a64/cg64/target and master trees do
# not. ir.c leads: it owns the shared IR type definitions. macho was dropped
# above when present; hoststub.c then supplies its undefined symbols.
MODULES=$(cd src && LC_ALL=C ls *.c | sed 's/\.c$//' | tr '\n' ' ')
SRCS=""
for m in $MODULES; do
    [ "$m" = macho ] && continue
    SRCS="$SRCS v0/$m.c"
done
[ "$HAS_MACHO" = 1 ] && SRCS="$SRCS runtime/hoststub.c"

# Probe whether this compiler knows --target. The usage text is printed on
# stdout together with a non-zero exit status, so the probe must swallow that
# status explicitly: under `set -o pipefail` a bare `fakecc | grep -q` reports
# failure (upstream's non-zero rc wins over grep's match) and --target would
# be silently dropped, leaving the arm64-macos host default in place.
TARGET_FLAG=""
if grep -q -- '--target' <("$S0" 2>&1 || true); then
    TARGET_FLAG='--target=x86_64-linux'
fi

mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086 # SRCS/TARGET_FLAG intentionally word-split
"$S0" $TARGET_FLAG -O0 $SRCS -o "$OUT" 2>v0/final.err || \
    { cat v0/final.err >&2; exit 1; }

# --- 5. verify the artifact is a STATIC ET_EXEC ------------------------------
# These MUST be plain `if ! ...` statements, not bare `cmd | grep -q` lines:
# a `cmd | grep -q && echo` compound swallows grep's failure status, so
# `set -e` never fires and a mis-targeted or dynamically linked artifact
# sails through silently. fakeos has no ld.so, so a PT_INTERP artifact would
# fail at execve inside the OS.
if ! file "$OUT" | grep -q "ELF 64-bit"; then
    echo "artifact is not a 64-bit ELF:" >&2
    file "$OUT" >&2
    exit 1
fi
if ! file "$OUT" | grep -q "statically linked"; then
    echo "artifact is not statically linked (fakeos has no ld.so):" >&2
    file "$OUT" >&2
    echo "=> a symbol is still unresolved; it became a PLT entry. Check that" >&2
    echo "   hoststub.c covers every non-static symbol in src/macho.c." >&2
    exit 1
fi

# Belt-and-braces: prove no PLT/dynamic relocation survived. `file` only
# pattern-matches the PT_INTERP string, so also read the ELF directly and
# fail on any undefined dynamic symbol.
python3 - "$OUT" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
shoff = struct.unpack_from('<Q', d, 0x28)[0]
shentsize = struct.unpack_from('<H', d, 0x3a)[0]
shnum = struct.unpack_from('<H', d, 0x3c)[0]
shstrndx = struct.unpack_from('<H', d, 0x3e)[0]
secs = []
for i in range(shnum):
    o = shoff + i * shentsize
    secs.append(struct.unpack_from('<IIQQQQIIQQ', d, o))
sbase = secs[shstrndx][4]
def nm(x):
    s = d[sbase + x:]
    return s[:s.index(b'\0')].decode()
undef = []
for s in secs:
    if nm(s[0]) != '.dynsym' or s[5] == 0:
        continue
    dstr = secs[s[6]][4]
    for k in range(s[5] // 24):
        nameoff, info, other, shndx, val, size = struct.unpack_from(
            '<IBBHQQ', d, s[4] + k * 24)
        if shndx == 0 and nameoff:
            b = d[dstr + nameoff:]
            undef.append(b[:b.index(b'\0')].decode())
if undef:
    print("unresolved dynamic symbols: " + ", ".join(undef), file=sys.stderr)
    sys.exit(1)
PYEOF
echo "built static native fakecc: $OUT ($(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes)"
