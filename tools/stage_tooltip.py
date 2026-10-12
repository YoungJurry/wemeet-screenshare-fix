#!/usr/bin/env python3
"""Stage an unmodified, hash-validated framework for the layout lifetime guard.

No Tencent instructions are rewritten. A private copy pins the exact supported
ELF for the read-only sandbox mount. See docs/tooltip-reentrancy.md.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path

ORIGINAL_SHA256 = "2118f17803de4fb8e02b44d273b6301f206e893f6d1024495b605648c510ed2a"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stage(input_path: Path, output_path: Path) -> None:
    if input_path.resolve() == output_path.resolve():
        raise ValueError("refusing an in-place staging operation")
    if output_path.exists() and os.path.samefile(input_path, output_path):
        raise ValueError("refusing staging through a hard link")
    original = input_path.read_bytes()
    actual = digest(original)
    if actual != ORIGINAL_SHA256:
        raise ValueError(f"input SHA-256 mismatch: {actual}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(output_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(original)
            os.fchmod(output.fileno(), input_path.stat().st_mode & 0o777)
    except BaseException:
        output_path.unlink(missing_ok=True)
        raise
    print(f"framework_sha256={actual}")
    print("framework_instructions=unmodified")
    print(f"output={output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        stage(args.input, args.output)
    except (ValueError, OSError) as error:
        raise SystemExit(f"Refusing tooltip staging: {error}") from error


if __name__ == "__main__":
    main()
