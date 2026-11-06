#!/usr/bin/env bash
# Regression: clean fakeos make must not invoke host C toolchain / nasm.
# Creates temporary PATH deny wrappers (log + fail); does not uninstall tools.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DENY=$(mktemp -d /tmp/fakeos-deny-XXXXXX)
LOG=$DENY/deny.log
: >"$LOG"
export FAKEOS_DENY_LOG=$LOG

for name in cmake cmake3 ccmake cpack ctest \
            cc c89 c99 c2x gcc gcc-14 gcc-13 gcc-12 clang clang-20 clang-19 clang++ g++ \
            cpp gcc-ar gcc-nm gcc-ranlib \
            as ld ld.bfd ld.gold ld.lld lld \
            objcopy objdump strip ranlib ar nm \
            nasm yasm \
            x86_64-elf-as x86_64-elf-ld x86_64-elf-objcopy \
            x86_64-linux-gnu-as x86_64-linux-gnu-ld x86_64-linux-gnu-objcopy \
            x86_64-linux-gnu-gcc x86_64-linux-gnu-g++ x86_64-linux-gnu-cpp; do
  cat >"$DENY/$name" <<'WRAP'
#!/bin/bash
echo "DENY $(basename "$0") argv=$* cwd=$PWD ppid=$PPID parent=$(ps -o comm= -p $PPID 2>/dev/null)" >>"${FAKEOS_DENY_LOG}"
echo "fakeos-deny: blocked $(basename "$0") $*" >&2
exit 127
WRAP
  chmod +x "$DENY/$name"
done

# Keep python3/qemu/make/bash/coreutils reachable; put deny first.
export PATH="$DENY:/usr/bin:/bin:/usr/local/bin:${HOME}/.local/bin:${HOME}/.local/cmake/bin:${PATH}"
FCC=${FCC:-$ROOT/../fakecc/build/fakecc}
[ -x "$FCC" ] || { echo "seed FCC missing: $FCC" >&2; exit 1; }

echo "[smoke] deny wrappers in $DENY"
echo "[smoke] seed FCC=$FCC ($(git -C "$(dirname "$FCC")/.." rev-parse --short HEAD 2>/dev/null || echo unknown))"
cd "$ROOT"
rm -rf "$ROOT/build" /tmp/fakeos-m5-fakecc
make -j"$(nproc)" FCC="$FCC"
if [ -s "$LOG" ]; then
  echo "[smoke] FAIL: host toolchain invocations:" >&2
  cat "$LOG" >&2
  exit 1
fi
echo "[smoke] OK: clean make with zero deny hits; log=$LOG"
ls -la "$ROOT/build/fakeos.elf" "$ROOT/build/m5stage/bin/fakecc"
python3 -c "import pathlib; p=pathlib.Path(\"$ROOT/build/m5stage/bin/fakecc\"); d=p.read_bytes(); assert d[:4]==b\"\\x7fELF\" and d[4]==2, d[:8]; print(\"staged ELF64\", p.stat().st_size)"
# Prove embed is not a byte-copy of the seed
if cmp -s "$FCC" "$ROOT/build/m5stage/bin/fakecc"; then
  echo "[smoke] FAIL: staged /bin/fakecc is identical to seed (must be freshly self-hosted)" >&2
  exit 1
fi
echo "[smoke] staged fakecc differs from seed (freshly self-hosted)"
