"""Bounded, integrity-checked transport for the complete bootstrap artifact."""
import base64
import hashlib
import os
from pathlib import Path
import re
import subprocess
import struct
import time

MAX_IMAGE_BYTES = 64 * 1024 * 1024
MIN_BINARY_BYTES_PER_SECOND = 32 * 1024
MAX_TRANSFER_SECONDS = 2400
STALL_SECONDS = 60
CHUNK = 12288
END = b"===DUMP/END==="
DONE = re.compile(rb"(?:^|[\r\n]|===DUMP/END===)dumpbin: sent ([0-9]+) bytes\r?\n")
FRAME = re.compile(
    rb"===DUMP/BEGIN name=fakeos-new len=([0-9]+)===\s*(.*?)===DUMP/END===",
    re.S,
)


def dump_transfer_budget(expected_size):
    """Host-validated bytes only: 60s startup + 32KiB/s, floor120, cap2400.

    The measured UART rate is about62KiB/s binary. This allows roughly a
    factor2 throughput margin, not an unbounded guest-controlled deadline.
    """
    if type(expected_size) is not int or not 0 < expected_size <= MAX_IMAGE_BYTES:
        raise ValueError("expected bootstrap size outside trusted 1..64MiB bound")
    seconds = 60 + (expected_size + MIN_BINARY_BYTES_PER_SECOND - 1) // MIN_BINARY_BYTES_PER_SECOND
    return min(MAX_TRANSFER_SECONDS, max(120, seconds))


def prepare_reference_image():
    """Use the actual built linker on trusted staged refs/asm (Linux CI).

    This native run is ONLY an oracle for byte count/hash. It never substitutes
    for guest compilation, linking, serial transfer, or second-boot acceptance.
    """
    source = Path("user/kbuild.c").read_text()
    block = re.search(r"static const struct mod_entry modules\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if block is None:
        raise ValueError("cannot find bootstrap module manifest")
    modules = re.findall(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*\}', block[1])
    if (len(modules) != 40 or len(set(modules)) != 40
            or modules[-1] != ("kernel", "kmain")
            or ("kernel", "native") not in modules):
        raise ValueError("reference must include all 40 modules, including native and kmain")
    paths = ["asm/boot.o"] + [f"ref/{p}/{n}.o" for p, n in modules]
    paths += [f"asm/{n}.o" for n in (
        "desc", "io", "sysentry", "switch", "ap_tramp_blob", "userblob", "m5stub")]
    args = Path("build/fakeos-reference.args").resolve()
    output = Path("build/fakeos-reference.elf").resolve()
    args.write_text("\n".join(paths) + "\n")
    subprocess.run([str(Path("build/user/ldfake.elf").resolve()), "-o", str(output),
                    "@" + str(args)], cwd="build/m5stage", check=True, timeout=60)
    size = output.stat().st_size
    dump_transfer_budget(size)  # Validate the trusted oracle before reading it.
    data = output.read_bytes()
    if data[:5] != b"\x7fELF\x01" or data[0x1000:0x1004] != bytes.fromhex("02b0ad1b"):
        raise ValueError("native reference is not a multiboot ELF32 image")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    if phnum != 2 or phoff + phnum * 32 > size:
        raise ValueError("invalid native reference program headers")
    headers = [struct.unpack_from("<IIIIIIII", data, phoff + i * 32) for i in range(phnum)]
    if any(p[0] != 1 or p[5] < p[4] for p in headers) or max(p[1] + p[4] for p in headers) != size:
        raise ValueError("reference size does not match loadable file ranges")
    return size, hashlib.sha256(data).hexdigest()


def capture_dump(stream, selector, expected_size):
    """Finite total + complete-frame stall + byte bounds; never claims validity.

    Integrity is independently checked by decode_dumpbin. Incoming noise does
    not renew the stall timer: only a completed frame advances it.
    """
    budget = dump_transfer_budget(expected_size)
    deadline = time.monotonic() + budget
    progress = time.monotonic()
    # Base64 plus per-frame headers, bounded extra console noise.
    wire_limit = ((expected_size + 2) // 3) * 4 + ((expected_size + CHUNK - 1) // CHUNK) * 128 + 1048576
    chunks, suffix, count = [], b"", 0
    reason = "eof"
    while True:
        now = time.monotonic()
        if now >= deadline:
            reason = "deadline"
            break
        if now - progress >= STALL_SECONDS:
            reason = "stalled"
            break
        ready = selector.select(min(0.2, deadline - now, STALL_SECONDS - (now - progress)))
        if not ready:
            continue
        data = os.read(stream.fileno(), 32768)
        if not data:
            break
        count += len(data)
        if count > wire_limit:
            reason = "output limit"
            break
        chunks.append(data)
        scan = suffix + data
        end = scan.rfind(END)
        if end >= 0 and end + len(END) > len(suffix):
            progress = time.monotonic()
        if DONE.search(scan):
            reason = "completion line"
            break
        suffix = scan[-128:]
    return b"".join(chunks).decode("utf-8", "surrogateescape"), reason


def decode_dumpbin(captured, expected_size, expected_digest):
    """Reject missing/truncated/corrupt/reordered output, even complete prefixes."""
    dump_transfer_budget(expected_size)
    raw = captured if isinstance(captured, bytes) else captured.encode("utf-8", "surrogateescape")
    matches = list(FRAME.finditer(raw))
    done = DONE.findall(raw)
    if len(done) != 1 or int(done[0]) != expected_size:
        raise ValueError("missing completion line or wrong total size")
    if (not matches or raw.count(b"===DUMP/BEGIN") != len(matches)
            or raw.count(END) != len(matches)):
        raise ValueError("missing/incomplete dump frame")
    chunks, declared = [], 0
    for match in matches:
        n = int(match[1])
        if not 0 < n <= CHUNK or declared + n > expected_size:
            raise ValueError("dump frame outside expected size")
        data = base64.b64decode(b"".join(match[2].split()), validate=True)
        if len(data) != n:
            raise ValueError("dump frame length mismatch")
        declared += n
        chunks.append(data)
    blob = b"".join(chunks)
    if declared != expected_size or hashlib.sha256(blob).hexdigest() != expected_digest:
        raise ValueError("incomplete or corrupt bootstrap image (size/SHA256)")
    return blob, len(matches), declared
