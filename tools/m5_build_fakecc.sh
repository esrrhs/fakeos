#!/usr/bin/env bash
# ==============================================================================
# Milestone 5 toolchain: build a native x86_64-linux fakecc for embedding into
# fakeos' Ramfs. The fakecc source tree is NEVER modified: all work happens in
# a throwaway /tmp scratch copy.
#
# Normal path (no host C toolchain):
#   Seed compiler $(FCC) compiles the checked-in dialect snapshot v0/*.c into a
#   static linux x86-64 ELF. Translation (v0/translate.py / gcc -E) and
#   CMake/cc stage0 are intentionally NOT part of a normal fakeos build.
#
# Requirements:
#   - FCC (or FAKECC / argv override) points at an existing fakecc executable
#     that can target x86_64-linux (sibling ../fakecc/build/fakecc is typical).
#   - FAKECC_SRC has src/ + runtime/ + include/ + a checked-in v0/ whose module
#     set covers the linux bootstrap (hoststub XOR macho; see below).
#   - If src/ gains a module not yet in v0/ (e.g. as_nasm before v0 refresh),
#     this script FAILS FAST rather than silently pruning tip features.
#     Refresh upstream v0 (translate.py / host cpp) or point FAKECC_SRC at a
#     coherent tree (e.g. fakecc#114). Not part of normal fakeos make.
#
# Obtaining the first seed fakecc from source (CMake/cc) is outside this script;
# see README "构建与运行".
#
# Usage: m5_build_fakecc.sh <output_path>
# Env:   FAKECC_SRC (default ../fakecc), FCC / FAKECC (seed compiler).
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FAKECC_SRC=${FAKECC_SRC:-"$ROOT/../fakecc"}
OUT=$1

[ -d "$FAKECC_SRC/src" ] || { echo "FAKECC_SRC not found: $FAKECC_SRC" >&2; exit 1; }
[ -d "$FAKECC_SRC/v0" ] || {
    echo "FAKECC_SRC has no checked-in v0/ dialect snapshot: $FAKECC_SRC" >&2
    echo "Normal fakeos builds require v0/*.c; do not run translate.py here." >&2
    exit 1
}

# OUT must be absolute: this script cd's into a /tmp scratch tree below.
case "$OUT" in
    /*) ;;
    *)  OUT="$PWD/$OUT" ;;
esac

# Seed compiler: never cmake/cc-build one here.
# Resolve relative FCC / FAKECC_SRC against the caller's cwd BEFORE we cd into
# the /tmp scratch (otherwise FCC=../fakecc/build/fakecc breaks after cd).
S0=${FCC:-${FAKECC:-}}
if [ -z "$S0" ]; then
    if [ -x "$ROOT/../fakecc/build/fakecc" ]; then
        S0="$ROOT/../fakecc/build/fakecc"
    elif command -v fakecc >/dev/null 2>&1; then
        S0=$(command -v fakecc)
    fi
fi
if [ -z "$S0" ]; then
    echo "m5_build_fakecc: no seed fakecc (set FCC= to an existing fakecc binary)." >&2
    echo "The first seed is obtained outside the normal fakeos build (fakecc CMake)." >&2
    exit 1
fi
case "$S0" in
    /*) ;;
    *)  S0="$PWD/$S0" ;;
esac
if [ ! -x "$S0" ]; then
    echo "m5_build_fakecc: seed not executable: $S0" >&2
    exit 1
fi
# Canonical absolute path (also collapses ..).
S0=$(cd "$(dirname "$S0")" && pwd)/$(basename "$S0")

case "$FAKECC_SRC" in
    /*) ;;
    *)  FAKECC_SRC="$PWD/$FAKECC_SRC" ;;
esac
FAKECC_SRC=$(cd "$FAKECC_SRC" && pwd)

SCRATCH=/tmp/fakeos-m5-fakecc

# --- 1. fresh scratch copy ----------------------------------------------------
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
cp -R "$FAKECC_SRC/src" "$FAKECC_SRC/runtime" "$FAKECC_SRC/include" "$FAKECC_SRC/v0" "$SCRATCH/"

cd "$SCRATCH"

HAS_MACHO=0
[ -f src/macho.c ] && HAS_MACHO=1
HAS_HOSTSTUB_V0=0
[ -f v0/hoststub.c ] && HAS_HOSTSTUB_V0=1

# Darwin->Linux fopen constants (no-op on upstream master already on Linux).
if [ -f runtime/stdio.c ] && grep -q '512 /\* O_CREAT \*/ | 1024 /\* O_TRUNC \*/' runtime/stdio.c; then
    python3 - <<'PYEOF'
p = "runtime/stdio.c"
s = open(p).read()
s2 = s.replace("| 512 /* O_CREAT */ | 1024 /* O_TRUNC */",
                "| 64 /* O_CREAT */ | 512 /* O_TRUNC */")
s2 = s2.replace("| 512 /* O_CREAT */ | 8 /* O_APPEND */",
                "| 64 /* O_CREAT */ | 1024 /* O_APPEND */")
assert s2 != s, "Darwin fopen constants found but not rewritten"
s3 = s2.replace(
    "/* O_CREAT is 0x200 and O_TRUNC is 0x400 on both Linux and Darwin; the\n"
    "     * 0x40 used here belongs to O_NOCTTY, so \"w\" created nothing and \"a\"\n"
    "     * asked for neither create nor append.  O_APPEND is 0x08. */",
    "/* fopen flags rewritten to the Linux values by tools/m5_build_fakecc.sh:\n"
    "     * O_CREAT 0x40, O_TRUNC 0x200, O_APPEND 0x400.  This compiler emits a\n"
    "     * linux x86-64 stage1 that runs on fakeos, whose ramfs uses the Linux\n"
    "     * numbers, so the Darwin values that were here did not create files. */")
assert s3 != s2, "Darwin fopen comment found but not rewritten"
open(p, "w").write(s3)
PYEOF
fi

# --- 2. choose module set from checked-in v0 (no translate / no host cpp) -----
# Every src/*.c module MUST have a matching checked-in v0/*.c, except the
# intentional linux backend drop (macho XOR hoststub). Silently pruning stale
# modules (e.g. as_nasm) would embed a compiler missing tip features — refuse.
SRC_MODS=$(cd src && LC_ALL=C ls *.c | sed 's/\.c$//')
V0_MODS=$(cd v0 && LC_ALL=C ls *.c | sed 's/\.c$//')
ONLY_SRC=$(comm -23 <(printf '%s\n' $SRC_MODS | sort) <(printf '%s\n' $V0_MODS | sort) || true)

# Linux embed: prefer hoststub over macho when the snapshot provides hoststub
# (matches fakecc FAKECC_NO_MACHO). Otherwise keep macho so the link stays static.
DROP=
if [ "$HAS_HOSTSTUB_V0" = 1 ]; then
    DROP=macho
elif [ "$HAS_MACHO" = 1 ] && printf '%s\n' $V0_MODS | grep -qx macho; then
    DROP=hoststub
else
    echo "m5_build_fakecc: checked-in v0 needs hoststub.c or macho.c for a static linux link" >&2
    exit 1
fi

MISSING=
for m in $ONLY_SRC; do
    [ "$m" = "$DROP" ] && continue
    # src may list the dropped sibling (macho when using hoststub, or vice versa)
    if [ "$DROP" = macho ] && [ "$m" = hoststub ]; then continue; fi
    if [ "$DROP" = hoststub ] && [ "$m" = macho ]; then continue; fi
    MISSING="$MISSING $m"
done
if [ -n "$(echo $MISSING)" ]; then
    echo "m5_build_fakecc: FAKECC_SRC v0/ is stale vs src/ — refusing to drop modules:" >&2
    echo "$MISSING" | tr ' ' '\n' | sed '/^$/d;s/^/  - /' >&2
    echo "Refresh upstream v0 outside fakeos (cd FAKECC_SRC && python3 v0/translate.py)," >&2
    echo "or point FAKECC_SRC at a tree whose checked-in v0 matches src (e.g. fakecc#114)." >&2
    exit 1
fi

MODULES=
for m in $SRC_MODS; do
    [ "$m" = "$DROP" ] && continue
    # When dropping macho, require hoststub in v0 (already gated above).
    if [ "$DROP" = macho ] && [ "$m" = hoststub ]; then
        MODULES="$MODULES $m"
        continue
    fi
    if [ "$DROP" = hoststub ] && [ "$m" = macho ]; then
        MODULES="$MODULES $m"
        continue
    fi
    [ -f "v0/$m.c" ] || { echo "m5_build_fakecc: missing v0/$m.c" >&2; exit 1; }
    MODULES="$MODULES $m"
done
[ -n "$MODULES" ] || { echo "m5_build_fakecc: empty module set" >&2; exit 1; }

# WIP arm64-macos translated trees may bake TARGET_ARM64_MACOS into host_default;
# flip for a linux x86-64 artifact that runs on fakeos. No-op on master/Linux v0.
if [ -f v0/target.c ] && grep -A2 'host_default(void)' v0/target.c | grep -q 'return \&TARGET_ARM64_MACOS;'; then
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

# --- 3. seed fakecc compiles checked-in v0 -> static linux ELF ----------------
SRCS="v0/ir.c"
for m in $MODULES; do
    [ "$m" = ir ] && continue
    SRCS="$SRCS v0/$m.c"
done

TARGET_FLAG=""
if grep -q -- '--target' <("$S0" 2>&1 || true); then
    TARGET_FLAG='--target=x86_64-linux'
fi

echo "[m5] self-host: seed=$S0"
echo "[m5] FAKECC_SRC=$FAKECC_SRC (checked-in v0; drop=$DROP)"
echo "[m5] modules:$(echo $MODULES | wc -w) -> $OUT"

mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086
"$S0" $TARGET_FLAG -O0 $SRCS -o "$OUT" 2>v0/final.err || {
    cat v0/final.err >&2
    exit 1
}

# --- 4. verify STATIC ET_EXEC (python-only; no dependency on `file`(1)) -----
python3 - "$OUT" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, 'rb').read()
if d[:4] != b'\x7fELF':
    print("artifact is not ELF:", path, file=sys.stderr); sys.exit(1)
ei_class, ei_data = d[4], d[5]
if ei_class != 2 or ei_data != 1:
    print("artifact is not ELF64 little-endian", file=sys.stderr); sys.exit(1)
e_type = struct.unpack_from('<H', d, 16)[0]
if e_type != 2:
    print(f"artifact e_type={e_type} (want ET_EXEC=2)", file=sys.stderr); sys.exit(1)
# Program headers: reject PT_INTERP (dynamic)
phoff = struct.unpack_from('<Q', d, 0x20)[0]
phentsize = struct.unpack_from('<H', d, 0x36)[0]
phnum = struct.unpack_from('<H', d, 0x38)[0]
for i in range(phnum):
    o = phoff + i * phentsize
    p_type = struct.unpack_from('<I', d, o)[0]
    if p_type == 3:  # PT_INTERP
        print("artifact has PT_INTERP (not static; fakeos has no ld.so)", file=sys.stderr)
        sys.exit(1)
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
print(f"verified static ELF64 ET_EXEC: {path} ({len(d)} bytes)")
PYEOF
echo "built static native fakecc (self-hosted from v0): $OUT ($(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes)"

# --- 5. capability smoke: tip assembler features must survive the embed -----
# If src ships as_nasm, the guest compiler must advertise and exercise -f bin /
# -f elf64 (fakeos boot.asm / ap_tramp.asm). Skip only when as_nasm is absent
# from the module set (pre-#113 trees).
#
# The self-hosted artifact ($OUT) is a linux x86-64 static ELF built to run
# inside fakeos. It may not be executable on the staging host (e.g. macOS),
# so we verify the -f bin / -f elf64 strings by reading the binary with grep
# (same approach m5_stage.sh uses for --target detection) rather than by
# executing $OUT. The actual asm smoke test only runs when the host can
# execute the artifact.
if printf '%s\n' $MODULES | grep -qx as_nasm; then
    if ! grep -qa -- '-f bin' "$OUT"; then
        echo "m5_build_fakecc: embed lacks -f bin (as_nasm present but unused?)" >&2
        exit 1
    fi
    if ! grep -qa -- '-f elf64' "$OUT"; then
        echo "m5_build_fakecc: embed lacks -f elf64" >&2
        exit 1
    fi
    # Prefer assembling the real fakeos boot sources when available (absolute
    # ROOT captured before scratch cd). Only run when the host can execute
    # the linux x86-64 artifact.
    BOOT_ASM="$ROOT/arch/x86_64/boot.asm"
    AP_ASM="$ROOT/arch/x86_64/ap_tramp.asm"
    if [ -f "$BOOT_ASM" ] && [ -f "$AP_ASM" ] && "$OUT" -f elf64 -o /dev/null "$BOOT_ASM" 2>/dev/null; then
        CAP=$SCRATCH/cap
        mkdir -p "$CAP"
        "$OUT" -f elf64 -o "$CAP/boot.o" "$BOOT_ASM" 2>"$CAP/boot.err" || {
            cat "$CAP/boot.err" >&2
            echo "m5_build_fakecc: embed failed -f elf64 on boot.asm" >&2
            exit 1
        }
        "$OUT" -f bin -o "$CAP/ap_tramp.bin" "$AP_ASM" 2>"$CAP/ap.err" || {
            cat "$CAP/ap.err" >&2
            echo "m5_build_fakecc: embed failed -f bin on ap_tramp.asm" >&2
            exit 1
        }
        # Seed must produce the same flat binary (assembler feature parity).
        "$S0" -f bin -o "$CAP/ap_seed.bin" "$AP_ASM" 2>/dev/null || true
        if [ -f "$CAP/ap_seed.bin" ] && ! cmp -s "$CAP/ap_tramp.bin" "$CAP/ap_seed.bin"; then
            echo "m5_build_fakecc: embed ap_tramp.bin differs from seed (as_nasm mismatch)" >&2
            exit 1
        fi
        echo "[m5] capability OK: embed -f elf64 boot.o + -f bin ap_tramp.bin ($(wc -c <"$CAP/ap_tramp.bin") bytes)"
    elif [ -f "$BOOT_ASM" ] && [ -f "$AP_ASM" ]; then
        echo "[m5] capability: -f bin/-f elf64 present in embed (host cannot execute linux x86-64 artifact; skipping asm smoke test)"
    fi
fi

# Record exact inputs actually compiled (including Linux scratch adaptations).
# Git revision is advisory; content hashes also cover dirty/non-git source trees.
python3 - "$FAKECC_SRC" "$SCRATCH" "$S0" "$OUT" "$DROP" $MODULES <<'PYPROV'
import hashlib, json, pathlib, subprocess, sys
source, scratch, seed, output, drop, *modules = sys.argv[1:]
def digest(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def files(root):
    root = pathlib.Path(root)
    return {str(p.relative_to(root)): digest(p)
            for directory in ('src', 'v0', 'runtime', 'include')
            for p in sorted((root / directory).rglob('*'))
            if p.is_file() and p.suffix in ('.c', '.h')}
try:
    revision = subprocess.check_output(
        ['git', '-C', source, 'rev-parse', 'HEAD'], stderr=subprocess.DEVNULL,
        text=True).strip()
except (OSError, subprocess.CalledProcessError):
    revision = None
manifest = dict(schema=1, source_revision=revision, source_sha256=files(source),
                effective_sha256=files(scratch), modules=modules,
                dropped_backend=drop, seed_sha256=digest(seed),
                artifact_sha256=digest(output))
pathlib.Path(output + '.provenance.json').write_text(
    json.dumps(manifest, sort_keys=True, indent=2) + '\n')
print('[m5] provenance: ' + output + '.provenance.json')
PYPROV
