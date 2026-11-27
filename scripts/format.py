#!/usr/bin/env python3
"""Run clang-format across all project C source and header files."""

import argparse
import os
import subprocess
import sys

EXTENSIONS = (".c", ".h")
SEARCH_DIRS = ("src", "tests")


def find_files(root_dir):
    files = []
    for d in SEARCH_DIRS:
        full_d = os.path.join(root_dir, d)
        if not os.path.exists(full_d):
            continue
        for parent, _, filenames in os.walk(full_d):
            for fn in filenames:
                if fn.endswith(EXTENSIONS):
                    files.append(os.path.join(parent, fn))
    files.sort()
    return files


def main():
    parser = argparse.ArgumentParser(description="Format C source code using clang-format")
    parser.add_argument("--check", action="store_true", help="Check formatting without modifying files")
    args = parser.parse_args()

    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    files = find_files(root_dir)
    if not files:
        print("No source files found.")
        return 0

    cmd = ["clang-format"]
    if args.check:
        cmd += ["--dry-run", "--Werror"]
    else:
        cmd += ["-i"]

    cmd += files
    try:
        res = subprocess.run(cmd)
        return res.returncode
    except FileNotFoundError:
        print("clang-format not found in PATH", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
