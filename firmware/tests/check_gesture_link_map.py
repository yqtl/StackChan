#!/usr/bin/env python3
"""Reject ESP-SR's legacy kernels being linked into ESP-DL hand inference."""

import re
import sys
from pathlib import Path


def main():
    link_map = Path(sys.argv[1]).read_text()
    # GNU ld's cross-reference table lists the defining archive on this line,
    # followed by indented lines for consumers. Inspect definitions only.
    kernels = re.findall(
        r"^(dl_tie728_s8_(?:unaligned_)?conv2d_\w+)\s+(\S+)",
        link_map,
        re.MULTILINE,
    )
    if not kernels:
        sys.exit("FAIL: no ESP32-S3 hand convolution definitions found in link map")
    wrong = [(symbol, archive) for symbol, archive in kernels
             if "espressif__esp-dl/" not in archive]
    if wrong:
        for symbol, archive in wrong:
            print(f"FAIL: {symbol} supplied by {archive}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS: {len(kernels)} hand convolution kernels supplied by ESP-DL")


if __name__ == "__main__":
    main()
