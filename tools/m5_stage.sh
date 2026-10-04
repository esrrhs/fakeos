#!/usr/bin/env bash
# ==============================================================================
# Milestone 5: stage the in-OS build tree under build/m5stage and emit
# build/m5blob.asm - one incbin per file plus a manifest table consumed by
# kernel/rootfs.c (no per-file extern declarations needed in C).
#
# Staged layout (relative path -> Ramfs path):
#   bin/fakecc                 /bin/fakecc      (native static compiler)
#   src/runtime/*.c            /src/runtime     (fakecc's own builtin runtime)
#   src/<pkg>/*.c              /src/<pkg>/...   (kernel C sources)
#   src/ping.c                 /src/ping.c
#   ref/<pkg>/*.o              /ref/<pkg>/...   (host reference objects)
#   asm/*.o                    /asm/...         (prebuilt nasm objects)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FAKECC_SRC=${FAKECC_SRC:-"$ROOT/../fakecc"}
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

# Native compiler binary (rebuilt only when missing; the build script itself
# is idempotent from a clean scratch copy).
if [ ! -f "$STAGE/bin/fakecc" ]; then
    mkdir -p "$STAGE/bin"
    FAKECC_SRC="$FAKECC_SRC" bash "$SCRIPT_DIR/m5_build_fakecc.sh" "$STAGE/bin/fakecc"
fi

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

# Kernel C sources, preserving package directories.
for pkg in types arch drivers kernel mem fs; do
    mkdir -p "$STAGE/src/$pkg"
    cp "$ROOT/$pkg"/*.c "$STAGE/src/$pkg/"
done

# In-OS demo source.
cp "$ROOT/user/progs/ping.c" "$STAGE/src/ping.c"

# Host reference objects.
#
# The in-OS compiler is fakecc stage 1: the current fakecc sources compiled
# to a static linux x86-64 binary by the clang-built fakecc "stage 0" (see
# tools/m5_build_fakecc.sh). Stage 0 runs natively on this (arm64 macOS)
# host while emitting x86-64 linux code, so use it to compile every kernel
# module here: byte-identical output between stage 0 and the self-hosted stage 1
# is exactly the determinism AC-5 proves inside the OS. M5_S0 overrides the
# stage-0 location; when it is unavailable, fall back to the objects already
# built for the kernel image in $BUILD (produced by the installed fakecc).
#
# The fallback also covers a stage 0 that exists but cannot compile the
# sources -- the CI runner builds stage 0 from upstream fakecc master, whose
# compiler crashes (SIGSEGV) on this kernel's sources while the installed
# fakecc compiles them fine. Those objects came from the very compiler the
# in-OS fakecc is compared against, so the byte comparison stays meaningful.
M5_S0="${M5_S0:-/tmp/fakeos-m5-fakecc/s0host/fakecc}"
for src in $(find "$STAGE/src" -name '*.c' ! -path '*/runtime/*' ! -name 'ping.c' | sort); do
    rel=${src#$STAGE/src/}
    n=$(basename "$rel" .c)
    out_rel=$(dirname "$rel")/$n.o
    mkdir -p "$STAGE/ref/$(dirname "$rel")"
    if [ -x "$M5_S0" ]; then
        if ! ( cd "$ROOT" && \
               FAKECC_PKG="$ROOT" "$M5_S0" -O0 $REF_TARGET_FLAG \
                   -c "$src" -o "$STAGE/ref/$out_rel" ); then
            echo "stage0 failed on $rel; falling back to build/$n.o" >&2
            rm -f "$STAGE/ref/$out_rel"
            if [ -f "$BUILD/$n.o" ]; then
                cp "$BUILD/$n.o" "$STAGE/ref/$out_rel"
            else
                echo "no reference object available for $rel" >&2
                exit 1
            fi
        fi
    else
        cp "$BUILD/$n.o" "$STAGE/ref/$out_rel"
    fi
done

# Prebuilt nasm objects (no in-OS assembler in this milestone).
for a in boot desc io sysentry switch; do
    cp "$BUILD/$a.o" "$STAGE/asm/$a.o"
done
# Userland blob (init/sh/hello/cat/ls/kbuild/ldfake + motd/environ):
# the rebuilt kernel links it so the second-boot image keeps an equivalent
# userspace (without the 3.5 MB compiler/source blob).
cp "$BUILD/userblob.o" "$STAGE/asm/userblob.o"

# Empty m5 blob table for the rebuilt image: rootfs_publish() walks
# m5_blob_table/count, and a zero-length table simply publishes no
# staged compiler files. Built on the host (no in-OS assembler).
STUB="$STAGE/asm/m5stub.s"
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

# --- emit build/m5blob.asm -----------------------------------------------------
ASM=$BUILD/m5blob.asm
{
    echo '; GENERATED by tools/m5_stage.sh - do not edit.'
    echo '[bits 64]'
    echo 'section .rodata'
    echo
    n=0
    while IFS= read -r f; do
        rel=${f#$STAGE/}
        sym=$(echo "$rel" | tr -c 'A-Za-z0-9' '_')
        echo "global blob_${sym}_start"
        echo "global blob_${sym}_end"
        echo "align 8"
        echo "blob_${sym}_start: incbin \"$f\""
        echo "blob_${sym}_end:"
        echo "p_$sym: db \"/$rel\", 0"
        n=$((n+1))
    done < <(find "$STAGE" -type f ! -name '.staged' | sort)
    echo
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
} > "$ASM"

echo "staged $n files into $STAGE; $ASM"
