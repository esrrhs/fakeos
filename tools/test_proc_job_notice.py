#!/usr/bin/env python3
"""Regression for real /proc-smoke job-notice parsing, not a guest substitute."""
import unittest
from m5_daily_proc_smoke import wait_job_pid


class ChunkedUART:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.consumed = []

    def wait(self, marker, timeout):
        out = ''
        while marker not in out:
            if not self.chunks:
                raise AssertionError('waiting past the complete job notice')
            chunk = self.chunks.pop(0)
            self.consumed.append(chunk)
            out += chunk
        return out


class JobNoticeTest(unittest.TestCase):
    def test_every_read_boundary_after_prefix(self):
        for ending in ('\r\n', '\n'):
            line = 'fakeos:/tmp$ command &\r\n[1] 123' + ending
            for boundary in range(line.index('[1] ') + 4, len(line) + 1):
                with self.subTest(ending=ending, boundary=boundary):
                    chunks = [line[:boundary]]
                    if line[boundary:]:
                        chunks.append(line[boundary:])
                    chunks.append('UNRELATED_LATER_OUTPUT\r\n')
                    uart = ChunkedUART(chunks)
                    self.assertEqual(wait_job_pid(uart.wait), '123')
                    self.assertEqual(uart.chunks, ['UNRELATED_LATER_OUTPUT\r\n'])

    def test_missing_or_malformed_pid_is_rejected(self):
        for text in ('[1] \r\n', '[1] nope\r\n'):
            with self.subTest(text=text):
                uart = ChunkedUART([text, '\n'])
                with self.assertRaises(AssertionError):
                    wait_job_pid(uart.wait)


if __name__ == '__main__':
    unittest.main()
