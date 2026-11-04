# Environment, flags, and Linux-only bootstrap (N1 / N2)

## Environment

| Variable | Role |
|----------|------|
| `FAKECC_RT` | Override path to builtin `runtime/` |
| `FAKECC_PKG` | Colon-separated package search path |
| `FAKECC_HOST_DEFAULT` | Override `host_default()` triple (e.g. `x86_64-linux` on Apple Silicon without patching sources) |

## CLI (stable)

| Flag | Role |
|------|------|
| `-nostdlib` / `-nodefaultlibs` | Do not link builtin `runtime/` |
| `--target=T` | `x86_64-linux` / `arm64-macos` / `x86_64-windows`; unknown → warning + host default (S4) |
| `-c` | Emit `ET_REL` (`.c` or `.S`) |

## Flags file (OS embed)

fakeos stores optional extra argv in `/etc/fakecc.flags` (may be empty).
kbuild prepends those tokens when invoking `/bin/fakecc`. Upstream does not
parse the file itself; keep the convention “whitespace-separated CLI flags”.

## Linux-only static bootstrap (N1)

CMake options (replace fakeos `/tmp` Mach-O / Darwin scratch patches):

```bash
cmake -DFAKECC_NO_MACHO=ON -DFAKECC_BOOTSTRAP_LINUX=ON ..
```

- `FAKECC_NO_MACHO` — link `src/hoststub.c` instead of `src/macho.c`
- `FAKECC_BOOTSTRAP_LINUX` — compile `host_default` as `x86_64-linux`

Runtime `fopen` open flags (`O_CREAT` / `O_TRUNC` / `O_APPEND`) come from
`runtime/stdlib_{linux,darwin,windows}.c` via `c_file_matches_target`, so a
Darwin host building a Linux target no longer needs to rewrite `stdio.c`.
