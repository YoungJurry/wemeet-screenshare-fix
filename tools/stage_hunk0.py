#!/usr/bin/env python3
"""Stage the Portal-ordering Hunk 0 for Wemeet 3.26.10.401.

This never edits the input in place. It validates the complete original hash and
every original byte sequence before writing a patched copy.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

# Each entry maps an accepted, byte-exact original module to the exact module
# produced by PATCHES. The AUR package and the official deb carry the same code
# but different RUNPATH bytes, so their hashes differ.
BUILDS = {
    # Arch/CachyOS wemeet-bin 3.26.10.401-5: deb payload after patchelf.
    "925e7c16a5d69adaa804d1ea55aac1edc4093aacb5324948ea6f38a71abd4e16":
        "075aef4471ed8ea87f0c9c95995981358087265987d05b656d3bf0b8a5f8f418",
    # Tencent official deb 3.26.10.401 x86_64 (also the source of the AUR files).
    "b3296906e5aba1a0b4297cd6e59b4ba19519e56de912b2ea0c5a23f3a252e7ed":
        "64bc0a7800bf92ff6515bb78965fff8d5d8bb9dc85a048611d5543b2ce321d8d",
}
PATCHES = {
    0x43D2FA: ("00", "5d"),
    0x43D331: ("88f7", "60fb"),
    0x43D336: ("83c718488b8588f7ffff48", "89fe4883c618e910000000"),
    0x43D357: ("0000", "e208"),
    0x43DC38: ("e8c305d9", "e9ecf6ff"),
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    if args.input.resolve() == args.output.resolve():
        raise SystemExit("refusing an in-place patch")

    original = args.input.read_bytes()
    actual = digest(original)
    expected_patched = BUILDS.get(actual)
    if expected_patched is None:
        raise SystemExit(f"input SHA-256 mismatch: {actual}")

    patched = bytearray(original)
    for offset, (before_hex, after_hex) in sorted(PATCHES.items()):
        before = bytes.fromhex(before_hex)
        after = bytes.fromhex(after_hex)
        if len(before) != len(after):
            raise SystemExit(f"length mismatch at {offset:#x}")
        found = original[offset : offset + len(before)]
        if found != before:
            raise SystemExit(
                f"byte mismatch at {offset:#x}: expected {before.hex()}, got {found.hex()}"
            )
        patched[offset : offset + len(after)] = after
        print(f"{offset:#010x}: {before.hex()} -> {after.hex()}")

    result = bytes(patched)
    actual_patched = digest(result)
    if actual_patched != expected_patched:
        raise SystemExit(f"patched SHA-256 mismatch: {actual_patched}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    args.output.chmod(args.input.stat().st_mode & 0o777)
    print(f"original_sha256={actual}")
    print(f"patched_sha256={expected_patched}")
    print(f"output={args.output}")


if __name__ == "__main__":
    main()
