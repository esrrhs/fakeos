#!/usr/bin/env python3
"""No QEMU required: bounded budgets and no acceptance of partial/corrupt dumps."""
import base64
from contextlib import nullcontext
import hashlib
import os
import selectors
import threading
import unittest
from unittest.mock import patch
import bootstrap_transfer as t


def framed(blob):
    parts = []
    for i in range(0, len(blob), t.CHUNK):
        b = blob[i:i + t.CHUNK]
        parts.append(f"===DUMP/BEGIN name=fakeos-new len={len(b)}===\n".encode()
                     + base64.b64encode(b) + b"\n===DUMP/END===\n")
    return b"".join(parts) + f"dumpbin: sent {len(blob)} bytes\r\n".encode()


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.blob = bytes(range(256)) * 101
        self.digest = hashlib.sha256(self.blob).hexdigest()
        self.raw = framed(self.blob)

    def decode(self, raw):
        return t.decode_dumpbin(raw, len(self.blob), self.digest)

    def test_budget(self):
        self.assertEqual(t.dump_transfer_budget(1), 120)
        self.assertEqual(t.dump_transfer_budget(21689248), 722)
        self.assertGreater(t.dump_transfer_budget(21689248), 350)
        self.assertLessEqual(t.dump_transfer_budget(t.MAX_IMAGE_BYTES), t.MAX_TRANSFER_SECONDS)
        self.assertEqual(t.dump_transfer_budget(t.MAX_IMAGE_BYTES), 2108)
        for n in [0, -1, True, 1.0, t.MAX_IMAGE_BYTES + 1, 10**100]:
            with self.subTest(n=n), self.assertRaises(ValueError):
                t.dump_transfer_budget(n)

    def test_complete_framing_and_hash(self):
        blob, count, declared = self.decode(self.raw)
        self.assertEqual(blob, self.blob)
        self.assertEqual((count, declared), (3, len(blob)))

    def test_actual_producer_adjacent_completion(self):
        # dumpbin's END write is14bytes (no newline). Accept this exact
        # producer grammar without accepting a partial frame or wrong hash.
        raw = self.raw.replace(b"===DUMP/END===\n", b"===DUMP/END===")
        self.assertEqual(self.decode(raw)[0], self.blob)
        self.assertIsNotNone(t.DONE.search(raw))

    def test_missing_completion_and_partial_frame(self):
        for raw in [self.raw[:-10], self.raw.split(b"dumpbin: sent")[0],
                    self.raw[:100] + f"\ndumpbin: sent {len(self.blob)} bytes\n".encode()]:
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                self.decode(raw)

    def test_complete_prefix_is_not_success(self):
        prefix = self.blob[:t.CHUNK]
        with self.assertRaises(ValueError):
            self.decode(framed(prefix))

    def test_invalid_length_base64_and_hash(self):
        variants = [self.raw.replace(b"len=12288", b"len=12287", 1),
                    self.raw.replace(b"AAECAw", b"!AECAw", 1),
                    self.raw.replace(b"AAECAw", b"BAECAw", 1),
                    self.raw.replace(b"len=12288", b"len=999999999", 1)]
        for raw in variants:
            with self.subTest(prefix=raw[:70]), self.assertRaises(ValueError):
                self.decode(raw)

    def test_wrong_or_unbounded_guest_total(self):
        raw = self.raw.replace(f"sent {len(self.blob)}".encode(), b"sent 999999999999999999")
        with self.assertRaises(ValueError):
            self.decode(raw)

    def test_reordered_or_duplicated_frames(self):
        frames = list(t.FRAME.finditer(self.raw))
        trailer = self.raw[frames[-1].end():]
        for order in [[1, 0, 2], [0, 1, 1, 2]]:
            # First two chunks happen to repeat for this fixture; make distinct
            # data below for the reorder case rather than infer validity.
            if order == [1, 0, 2]:
                blob = b"a" * t.CHUNK + b"b" * t.CHUNK
                raw = framed(blob); fs = list(t.FRAME.finditer(raw))
                raw = raw[fs[1].start():fs[1].end()] + b"\n" + raw[fs[0].start():fs[0].end()] + raw[fs[-1].end():]
                with self.assertRaises(ValueError):
                    t.decode_dumpbin(raw, len(blob), hashlib.sha256(blob).hexdigest())
            else:
                with self.assertRaises(ValueError):
                    self.decode(b"\n".join(self.raw[frames[i].start():frames[i].end()] for i in order) + trailer)

    def capture(self, data, **patches):
        r, w = os.pipe(); stream = os.fdopen(r, "rb", buffering=0)
        sel = selectors.DefaultSelector(); sel.register(stream, selectors.EVENT_READ)
        try:
            os.write(w, data)
            with (patch.multiple(t, **patches) if patches else nullcontext()):
                return t.capture_dump(stream, sel, 512)
        finally:
            sel.close(); stream.close(); os.close(w)

    def test_capture_complete(self):
        raw = framed(b"x" * 512).replace(b"===DUMP/END===\n", b"===DUMP/END===")
        out, reason = self.capture(raw)
        self.assertEqual(reason, "completion line")
        self.assertEqual(t.decode_dumpbin(out, 512, hashlib.sha256(b"x" * 512).hexdigest())[0], b"x" * 512)

    def test_stalled_incomplete_transfer(self):
        out, reason = self.capture(b"===DUMP/BEGIN name=fakeos-new len=512===\n", STALL_SECONDS=0.03)
        self.assertEqual(reason, "stalled")
        with self.assertRaises(ValueError):
            t.decode_dumpbin(out, 512, "0" * 64)

    def test_progress_cannot_extend_total_deadline(self):
        r, w = os.pipe(); stream = os.fdopen(r, "rb", buffering=0)
        sel = selectors.DefaultSelector(); sel.register(stream, selectors.EVENT_READ)
        stop = threading.Event()
        def noise():
            while not stop.wait(0.005):
                os.write(w, t.END + b"\n")
        writer = threading.Thread(target=noise); writer.start()
        try:
            with patch.object(t, "dump_transfer_budget", return_value=0.06), patch.object(t, "STALL_SECONDS", 0.03):
                out, reason = t.capture_dump(stream, sel, 512)
            self.assertEqual(reason, "deadline")
            with self.assertRaises(ValueError):
                t.decode_dumpbin(out, 512, "0" * 64)
        finally:
            stop.set(); writer.join(); sel.close(); stream.close(); os.close(w)


if __name__ == "__main__":
    unittest.main()
