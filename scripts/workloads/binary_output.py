#!/usr/bin/env python3
"""Deterministic direct-exec benchmark workload; stdout/stderr stay distinct."""
import os
import sys


def emit(fd, size, mask):
    offset = 0
    while offset < size:
        count = min(4096, size - offset)
        block = bytes(((offset + i) % 251) ^ mask for i in range(count))
        while block:
            written = os.write(fd, block)
            if written <= 0:
                raise RuntimeError("short output made no progress")
            block = block[written:]
        offset += count


if __name__ == "__main__":
    amount = int(sys.argv[1])
    if amount < 0 or amount > 16777216:
        raise ValueError("bounded benchmark size must be 0..16 MiB per stream")
    emit(1, amount, 0)
    emit(2, amount, 0xa5)
