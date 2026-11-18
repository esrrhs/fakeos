# Exact-CI-pin local acceptance

The source snapshot was audited against every tracked blob of
`dfb1c7b649480e2acfef2a0a02e8168b5b28ee38` in the existing local fakecc Git object
store (`existing-pinned-source-audit.json`, zero mismatches). This is the SAME
revision already pinned by fakeos CI; neither workflow nor compiler source changed.
The PATH-installed old compiler f74449... and a2158f private recovery are NOT used.

For a fresh private snapshot, `git -C /workspace/github/fakecc archive <full pin>`
can be extracted into an empty private directory, without touching its dirty tree.
Obtain the initial seed using the workflow's CMake Release process, installing
into an isolated prefix, NOT copying only its binary:

```sh
cmake -S /workspace/fakecc-dfb1c7b-src -B /private/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/private/prefix
cmake --build /private/build --target fakecc --parallel 2
cmake --install /private/build
FCC=/private/prefix/bin/fakecc \
FAKECC_SRC=/workspace/fakecc-dfb1c7b-src make clean
FCC=/private/prefix/bin/fakecc \
FAKECC_SRC=/workspace/fakecc-dfb1c7b-src make test
```

CMake/host C is ONLY the initial-seed exception, exactly as CI documents. Every
fakeos C/asm/link action and embedded v0 compiler build is then pure fakecc.
There is no NASM/host-compiler fallback added to normal fakeos make and no new
private `-f` compatibility patch. The pinned seed and staged compiler execute
real `-f elf64` boot and `-f bin` AP-trampoline checks.

This run used `/workspace/fakeos-ci-dfb1c7b-20261117/bin/fakecc`; source `toolchain.env`
for subsequent LOCAL invocations. Different prefixes can change ELF runtime-path
bytes and therefore seed hashes; source audit, CMake configuration, and real
capability tests—not just a filename—establish identity. Do not call any local
run an actual GitHub CI result. The actual full-gate command and true exit are in
`full-clean-acceptance.log` / `run-full-gate.sh`.

The prior failed build was retained at `build.before-ci-pin-20261117-2241`.
Minimal executable/machine-code comparison and failed/corrected barrier logs are
retained here; no old evidence was relabeled as acceptance of the new sources.
