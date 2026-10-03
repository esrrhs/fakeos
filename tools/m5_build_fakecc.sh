#!/usr/bin/env bash
# ==============================================================================
# Milestone 5 toolchain: build a native x86_64-linux fakecc for embedding into
# fakeos' Ramfs. The fakecc source tree is NEVER modified: all work happens in
# a throwaway /tmp scratch copy.
#
# Why the scratch shims exist:
#   - The checked-out fakecc tree is WIP. Its v0/ self-hosting snapshot lags
#     src/ (missing cg64/target/a64 modules); running translate.py regenerates
#     the dialect sources, but macho.c (macOS backend) needs sys/wait.h and
#     crashes the bootstrap compiler when translated. The Linux stage1 does not
#     need the Mach-O backend, so that one module is dropped and its three
#     entry points are provided by runtime/hoststub.c stubs.
#   - Output must be STATIC (fakeos has no ld.so); undefined symbols left by
#     the omitted macho module are fork/waitpid/execl/_exit, stubbed likewise.
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

# --- 2. host-side shim headers for the one-time gcc -E translation -----------
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

# --- 3. drop the Mach-O backend (crashes stage0 translation); stub it --------
rm -f v0/macho.c
sed -i '' '/#pragma pack/d' v0/*.c 2>/dev/null || true

# Translation runs cpp on the macOS host, so target.c's compile-time
#   #if defined(__APPLE__) && defined(__aarch64__)
# bakes the arm64-macos default into the generated dialect. This artifact is
# a linux x86-64 executable that will run ON linux x86-64 (fakeos), so flip
# host_default to x86_64-linux. target.c is the only source using those
# host macros; the explicit --target flag still overrides this default.
python3 - <<'PYEOF'
import re
p = "v0/target.c"
s = open(p).read()
s2, n = re.subn(r'(static const TargetDesc \*host_default\(void\) \{\s*)return &TARGET_ARM64_MACOS;',
                r'\1return &TARGET_X86_64_LINUX;', s)
assert n == 1, f"host_default patch matched {n} times"
open(p, "w").write(s2)
PYEOF

cat > runtime/hoststub.c <<'EOF'
/* Scratch-only stubs: Mach-O backend omitted from the linux stage1; the
 * codesign path is unreachable when targeting x86_64-linux. */
package runtime;

struct Buffer;
unsigned int macho_text_offset(void) { return 0; }
int macho_write_exec(struct Buffer *text, unsigned long entry_off,
                     const char *path) {
    (void)text; (void)entry_off; (void)path; return -1;
}
int macho_codesign(const char *path) { (void)path; return -1; }
int fork(void) { return -1; }
int waitpid(int pid, int *status, int options) {
    (void)pid; (void)status; (void)options; return -1;
}
void _exit(int code) { __syscall(60, (long)code); }
int execl(const char *path, ...) { (void)path; return -1; }
EOF

# --- 4. compile + self-link as a static linux x86-64 ELF ---------------------
# v0/ir.c first: it owns the shared IR type definitions.
MODULES="a64 ast cfg cg64 codegen common compiler debug dfp domtree emit ir \
lexer link main mem2reg opt parser pkg reg_arm64 regalloc scalar_opt sema target"
mkdir -p "$(dirname "$OUT")"
"$S0" --target=x86_64-linux -O0 v0/ir.c \
    $(for m in $MODULES; do [ "$m" = ir ] || echo v0/$m.c; done) \
    -o "$OUT" 2>v0/final.err || { cat v0/final.err >&2; exit 1; }

# --- 5. verify the artifact is a STATIC ET_EXEC ------------------------------
file "$OUT" | grep -q "ELF 64-bit"
file "$OUT" | grep -q "statically linked"
echo "built static native fakecc: $OUT ($(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes)"
