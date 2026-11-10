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
# Resolve relative overrides before cd, and use the SAME seed for make/tests.
FCC=${FCC:-$ROOT/../fakecc/build/fakecc}
case "$FCC" in /*) ;; *) FCC="$PWD/$FCC" ;; esac
FCC=$(cd "$(dirname "$FCC")" && pwd)/$(basename "$FCC")
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
# A real self-hosted seed can be byte-identical to its rebuilt output.
# Validate recorded input/output hashes, then compile AGAIN with the embed and
# require a fixed point instead of using seed inequality as a freshness proxy.
python3 - "$ROOT" "$FCC" "${FAKECC_SRC:-$ROOT/../fakecc}" <<'PYPROV'
import hashlib, json, pathlib, sys
root, seed, source = map(pathlib.Path, sys.argv[1:])
hashfile = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
m = json.loads((root / 'build/m5stage/etc/fakecc.provenance.json').read_text())
assert m['artifact_sha256'] == hashfile(root / 'build/m5stage/bin/fakecc')
assert m['seed_sha256'] == hashfile(seed)
for name, digest in m['source_sha256'].items():
    assert hashfile(source / name) == digest, name
assert 'as_nasm' in m['modules'] and 'main' in m['modules']
print('[smoke] provenance verified: revision=' + str(m['source_revision']))
PYPROV
FCC="$ROOT/build/m5stage/bin/fakecc" bash "$ROOT/tools/m5_build_fakecc.sh" "$DENY/fakecc-stage2"
cmp -s "$ROOT/build/m5stage/bin/fakecc" "$DENY/fakecc-stage2" || {
  echo "[smoke] FAIL: embedded compiler self-host did not reach fixed point" >&2; exit 1;
}
[ ! -s "$LOG" ] || { cat "$LOG" >&2; exit 1; }
echo "[smoke] OK: embed rebuild is byte-identical (fixed point); zero deny hits"
USAGE=$("$ROOT/build/m5stage/bin/fakecc" 2>&1 || true)
echo "$USAGE" | grep -q -- '-f bin' || { echo "[smoke] FAIL: staged fakecc lacks -f bin" >&2; exit 1; }
echo "$USAGE" | grep -q -- '-f elf64' || { echo "[smoke] FAIL: staged fakecc lacks -f elf64" >&2; exit 1; }
# Assemble real boot sources with the embed (host-exec of linux guest binary)
"$ROOT/build/m5stage/bin/fakecc" -f elf64 -o /tmp/smoke-boot.o "$ROOT/arch/x86_64/boot.asm"
"$ROOT/build/m5stage/bin/fakecc" -f bin -o /tmp/smoke-ap.bin "$ROOT/arch/x86_64/ap_tramp.asm"
"$FCC" -f bin -o /tmp/smoke-ap-seed.bin "$ROOT/arch/x86_64/ap_tramp.asm"
cmp -s /tmp/smoke-ap.bin /tmp/smoke-ap-seed.bin || { echo "[smoke] FAIL: embed ap_tramp != seed" >&2; exit 1; }
echo "[smoke] OK: staged embed -f bin/-f elf64 + ap_tramp matches seed"

[ ! -s "$LOG" ] || { cat "$LOG" >&2; exit 1; }
