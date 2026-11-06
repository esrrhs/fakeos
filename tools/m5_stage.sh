#!/usr/bin/env bash
# ==============================================================================
# Milestone 5: stage the in-OS build tree under build/m5stage and emit
# build/m5blob.S (or .asm) - one incbin per file plus a manifest table consumed by
# kernel/rootfs.c (no per-file extern declarations needed in C).
#
# Staged layout (relative path -> Ramfs path):
#   bin/fakecc                 /bin/fakecc      (native static compiler)
#   src/runtime/*.c            /src/runtime     (fakecc's own builtin runtime)
#   src/<pkg>/*.c              /src/<pkg>/...   (kernel C sources)
#   src/ping.c                 /src/ping.c
#   ref/<pkg>/*.o              /ref/<pkg>/...   (host reference objects)
#   asm/*.o                    /asm/...         (prebuilt host asm objects)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FAKECC_SRC=${FAKECC_SRC:-"$ROOT/../fakecc"}
# Pin reminder: tip after 2c1f4525 breaks rebuilt-kernel order-3 (BXOR).
echo "[m5] FAKECC_SRC=$FAKECC_SRC"
echo "[m5] FAKECC_SRC pin hint: restage only from fakecc @ 2c1f4525 (see docs/ldfake-order3-diagnosis.md)"
STAGE=$1
BUILD=$ROOT/build

[ -n "$STAGE" ] || { echo "usage: m5_stage.sh <stage_dir>" >&2; exit 2; }

# Resolve to an absolute path: the fakecc build script cd's into a /tmp
# scratch dir, so a relative OUT would silently land inside the scratch tree.
case "$STAGE" in
    /*) ;;
    *)  STAGE="$ROOT/$STAGE" ;;
esac
mkdir -p "$STAGE"
STAGE=$(cd "$STAGE" && pwd)

# Native compiler binary: always freshly self-hosted from checked-in v0 via the
# seed FCC (never CMake/cc, never copy the seed binary into place).
mkdir -p "$STAGE/bin"
FAKECC_SRC="$FAKECC_SRC" FCC="${FCC:-}" bash "$SCRIPT_DIR/m5_build_fakecc.sh" "$STAGE/bin/fakecc"

# Publish the extra CFLAGS this embedded fakecc understands. Multi-target
# builds accept --target; upstream master's single-backend compiler emits
# x86-64 ELF unconditionally and rejects the flag. /bin/kbuild reads this
# file at runtime, so the same kbuild binary works with either compiler.
#
# The probe must NOT execute the artifact: it is a linux x86-64 binary and the
# staging host is usually macOS, so running it fails with "exec format error"
# and the flag would be silently dropped -- which then makes the in-OS
# recompiles disagree with the host reference objects. Read the embedded usage
# string instead; it is present in the binary regardless of the build host.
mkdir -p "$STAGE/etc"
if grep -qa -- '--target' "$STAGE/bin/fakecc"; then
    echo '--target=x86_64-linux' > "$STAGE/etc/fakecc.flags"
    REF_TARGET_FLAG='--target=x86_64-linux'
else
    : > "$STAGE/etc/fakecc.flags"
    REF_TARGET_FLAG=''
fi

rm -rf "$STAGE/src" "$STAGE/ref" "$STAGE/asm"
mkdir -p "$STAGE/src/runtime" "$STAGE/ref" "$STAGE/asm"

# fakecc builtin runtime sources (compiled by fakecc inside the OS).
cp "$FAKECC_SRC"/runtime/*.c "$STAGE/src/runtime/"
# Same Darwin->Linux open(2) flag fix as m5_build_fakecc.sh step 1b, including
# the stale comment: the in-OS fakecc links this runtime when building ping, so
# /src/runtime must carry the Linux constants too.  No-op on upstream master.
if grep -q '512 /\* O_CREAT \*/ | 1024 /\* O_TRUNC \*/' "$STAGE/src/runtime/stdio.c"; then
    python3 - "$STAGE/src/runtime/stdio.c" <<'PYEOF'
import sys
p = sys.argv[1]
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
    "/* fopen flags rewritten to the Linux values by tools/m5_stage.sh:\n"
    "     * O_CREAT 0x40, O_TRUNC 0x200, O_APPEND 0x400.  The in-OS compiler\n"
    "     * targets linux x86-64 and fakeos' ramfs uses the Linux numbers. */")
assert s3 != s2, "Darwin fopen comment found but not rewritten"
open(p, "w").write(s3)
PYEOF
fi

# Kernel C sources, preserving package directories.
for pkg in types arch drivers kernel mem fs; do
    mkdir -p "$STAGE/src/$pkg"
    cp "$ROOT/$pkg"/*.c "$STAGE/src/$pkg/"
done

# In-OS demo source.
cp "$ROOT/user/progs/ping.c" "$STAGE/src/ping.c"

# Host reference objects.
#
# Prefer the freshly self-hosted STAGE/bin/fakecc (same binary the guest will
# run as /bin/fakecc) so /ref matches in-OS codegen. Override with M5_S0.
# If that binary cannot run on this host (e.g. staging on macOS), fall back to
# the seed FCC, then to build/*.o — but never mix producers across files.
#
# Trial-compile the whole tree with the chosen producer first and commit to a
# single source afterwards. Probing one file is not enough.
M5_S0="${M5_S0:-$STAGE/bin/fakecc}"
M5_SRCS=$(find "$STAGE/src" -name '*.c' ! -path '*/runtime/*' ! -name 'ping.c' | sort)
M5_S0_OK=0
if [ -n "$M5_S0" ] && [ -x "$M5_S0" ]; then
    M5_PROBE=$(mktemp -d)
    stage0_ok=1
    while IFS= read -r src; do
        rel=${src#"$STAGE/src/"}
        out_rel=$(dirname "$rel")/$(basename "$rel" .c).o
        mkdir -p "$M5_PROBE/$(dirname "$out_rel")"
        if ! ( cd "$ROOT" && FAKECC_PKG="$ROOT" \
                   "$M5_S0" -O0 $REF_TARGET_FLAG \
                   -c "$src" -o "$M5_PROBE/$out_rel" ) >/dev/null 2>&1; then
            echo "ref-compiler cannot compile $rel; using build/*.o as reference" >&2
            stage0_ok=0
            break
        fi
    done <<EOF
$M5_SRCS
EOF
    if [ "$stage0_ok" = 1 ]; then
        # Every trial object is valid: reuse them instead of recompiling.
        mkdir -p "$STAGE/ref"
        cp -R "$M5_PROBE/." "$STAGE/ref/"
        M5_S0_OK=1
    else
        # Drop the partial probe results by pointing the loop at build/*.o.
        # The scratch dir is removed below; $STAGE/ref is left alone because
        # the loop rewrites every entry in it from $BUILD anyway.
        M5_S0=""
        M5_S0_OK=0
    fi
    rm -rf "$M5_PROBE"
fi
while IFS= read -r src; do
    rel=${src#"$STAGE/src/"}
    n=$(basename "$rel" .c)
    out_rel=$(dirname "$rel")/$n.o
    # When the chosen ref compiler handled the tree the trial objects are already in place.
    [ -n "$M5_S0" ] && [ -f "$STAGE/ref/$out_rel" ] && continue
    mkdir -p "$STAGE/ref/$(dirname "$rel")"
    cp "$BUILD/$n.o" "$STAGE/ref/$out_rel"
done <<EOF
$M5_SRCS
EOF

# Prebuilt host asm objects (boot/ap_tramp/etc.; no in-OS assembler required here).
# ap_tramp_blob embeds the SIPI trampoline flat binary used by arch/smp.c.
for a in boot desc io sysentry switch ap_tramp_blob; do
    cp "$BUILD/$a.o" "$STAGE/asm/$a.o"
done
# Userland blob (init/sh/hello/cat/ls/kbuild/ldfake + motd/environ):
# the rebuilt kernel links it so the second-boot image keeps an equivalent
# userspace (without the 3.5 MB compiler/source blob).
cp "$BUILD/userblob.o" "$STAGE/asm/userblob.o"

# Empty m5 blob table for the rebuilt image: rootfs_publish() walks
# m5_blob_table/count, and a zero-length table simply publishes no
# staged compiler files. Built on the host (fakecc .S or nasm).
STUB="$STAGE/asm/m5stub.s"
USE_FAKECC_ASM="${USE_FAKECC_ASM:-1}"
FCC_BIN="${FCC:-fakecc}"

if [ "$USE_FAKECC_ASM" = "1" ]; then
    cat > "$STUB" <<'EOF'
    .section .rodata
    .globl m5_blob_table
m5_blob_table:
    .globl m5_blob_count
    .align 8
m5_blob_count:
    .quad 0
EOF
    FAKECC_PKG="$ROOT" "$FCC_BIN" -O0 -c "$STUB" -o "$STAGE/asm/m5stub.o"
else
    cat > "$STUB" <<'EOF'
[bits 64]
section .rodata
global m5_blob_table
m5_blob_table:
global m5_blob_count
align 8
m5_blob_count: dq 0
section .note.GNU-stack noalloc noexec nowrite progbits
EOF
    NASM_BIN="${NASM:-nasm}"
    "$NASM_BIN" -f elf64 -o "$STAGE/asm/m5stub.o" "$STUB"
fi

# --- emit build/m5blob.(S|asm) -------------------------------------------------
# Gas AT&T .S for fakecc; legacy nasm .asm when USE_FAKECC_ASM=0.
# Path strings use .byte (fakecc has no .ascii yet).
if [ "$USE_FAKECC_ASM" = "1" ]; then
    ASM=$BUILD/m5blob.S
else
    ASM=$BUILD/m5blob.asm
fi
{
    if [ "$USE_FAKECC_ASM" = "1" ]; then
        echo '/* GENERATED by tools/m5_stage.sh - do not edit. */'
        echo '    .section .rodata'
    else
        echo '; GENERATED by tools/m5_stage.sh - do not edit.'
        echo '[bits 64]'
        echo 'section .rodata'
    fi
    echo
    n=0
    while IFS= read -r f; do
        rel=${f#$STAGE/}
        sym=$(echo "$rel" | tr -c 'A-Za-z0-9' '_')
        if [ "$USE_FAKECC_ASM" = "1" ]; then
            echo "    .globl blob_${sym}_start"
            echo "    .globl blob_${sym}_end"
            echo "    .align 8"
            echo "blob_${sym}_start:"
            echo "    .incbin \"$f\""
            echo "blob_${sym}_end:"
            echo "p_$sym:"
            path="/$rel"
            printf '    .byte '
            first=1
            len=${#path}
            for ((i=0; i<len; i++)); do
                c=${path:i:1}
                o=$(printf '%d' "'$c")
                if [ $first -eq 1 ]; then first=0; else printf ', '; fi
                printf '%s' "$o"
            done
            printf ', 0\n'
        else
            echo "global blob_${sym}_start"
            echo "global blob_${sym}_end"
            echo "align 8"
            echo "blob_${sym}_start: incbin \"$f\""
            echo "blob_${sym}_end:"
            echo "p_$sym: db \"/$rel\", 0"
        fi
        n=$((n+1))
    done < <(find "$STAGE" -type f ! -name '.staged' | sort)
    echo
    if [ "$USE_FAKECC_ASM" = "1" ]; then
        # Pointer table + count live in .data so fakecc emits .rela.data with the
        # correct sh_info. Keeping them in .rodata historically produced
        # .rela.data entries whose offsets pointed into .rodata; the linker then
        # left every start/end pointer as 0 and rootfs published 0 staged files.
        echo '    .section .data'
        echo '    .globl m5_blob_table'
        echo 'm5_blob_table:'
        while IFS= read -r f; do
            rel=${f#$STAGE/}
            sym=$(echo "$rel" | tr -c 'A-Za-z0-9' '_')
            echo "    .quad p_$sym, blob_${sym}_start, blob_${sym}_end"
        done < <(find "$STAGE" -type f ! -name '.staged' | sort)
        echo
        echo '    .globl m5_blob_count'
        echo 'm5_blob_count:'
        echo "    .quad $n"
    else
        echo 'global m5_blob_table'
        echo 'm5_blob_table:'
        while IFS= read -r f; do
            rel=${f#$STAGE/}
            sym=$(echo "$rel" | tr -c 'A-Za-z0-9' '_')
            echo "    dq p_$sym, blob_${sym}_start, blob_${sym}_end"
        done < <(find "$STAGE" -type f ! -name '.staged' | sort)
        echo
        echo 'global m5_blob_count'
        echo 'm5_blob_count: dq '"$n"
        echo
        echo 'section .note.GNU-stack noalloc noexec nowrite progbits'
    fi
} > "$ASM"

echo "staged $n files into $STAGE; $ASM"
