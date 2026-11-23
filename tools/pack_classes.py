#!/usr/bin/env python3
"""Copy Java .class files to EXWord-friendly 8.3 .cls aliases.

All compiled Java .class files are left intact byte-for-byte. File names are
*not* used for class identity: the loader parses the actual JVM this_class
constant. Short .cls aliases prevent filename collisions and length limits.

Example:
  javac --release 8 -d classes Hello.java
  python3 tools/pack_classes.py classes build-exword/packages/ja/BTPRF

Input may be a directory tree or multiple directories. A deterministic alias
hash is used when the basename exceeds 8 characters or would collide.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct


def class_name(data):
    def u1():
        nonlocal pos
        if pos + 1 > len(data):
            raise ValueError("truncated classfile")
        b = data[pos]
        pos += 1
        return b

    def u2():
        return (u1() << 8) | u1()

    def u4():
        return (u2() << 16) | u2()

    def skip(n):
        nonlocal pos
        if pos + n > len(data):
            raise ValueError("truncated classfile")
        pos += n

    pos = 0
    if u4() != 0xCAFEBABE:
        raise ValueError("not a class file")
    skip(4)  # version
    count = u2()
    cp = [None] * count
    i = 1
    while i < count:
        tag = u1()
        if tag == 1:
            n = u2()
            if pos + n > len(data):
                raise ValueError("truncated UTF8")
            cp[i] = (tag, data[pos:pos+n].decode("utf-8", errors="strict"))
            skip(n)
        elif tag in (3, 4):
            skip(4)
        elif tag in (5, 6):
            skip(8)
            i += 1
        elif tag in (7, 8, 16, 19, 20):
            cp[i] = (tag, u2())
        elif tag in (9, 10, 11, 12, 17, 18):
            skip(4)
        elif tag == 15:
            skip(3)
        else:
            raise ValueError(f"unknown CP tag {tag}")
        i += 1
    skip(2)  # access
    idx = u2()
    if not cp[idx] or cp[idx][0] != 7:
        raise ValueError("missing this_class")
    name_index = cp[idx][1]
    if not cp[name_index] or cp[name_index][0] != 1:
        raise ValueError("missing class name")
    return cp[name_index][1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="target EXWord BTPRF directory")
    parser.add_argument("inputs", nargs="+", type=Path,
                        help="directories to recursively scan for .class files")
    parser.add_argument("--main", default="Main",
                        help="main JVM class for main.txt (default Main)")
    args = parser.parse_args()

    entries = {}
    for root in args.inputs:
        for path in sorted(root.rglob("*.class")):
            payload = path.read_bytes()
            name = class_name(payload)
            if name in entries and entries[name][0] != payload:
                raise SystemExit(f"duplicate JVM class name: {name}")
            entries[name] = (payload, path)
    if not entries:
        raise SystemExit("no class files found")

    args.output.mkdir(parents=True, exist_ok=True)
    occupied = {}
    mapping = []
    for name, (payload, original) in sorted(entries.items()):
        base = name.rsplit("/", 1)[-1]
        if re.fullmatch(r"[A-Za-z0-9_$]{1,8}", base):
            alias = base.upper()
        else:
            alias = hashlib.sha256(name.encode()).hexdigest()[:8].upper()
        if alias in occupied and occupied[alias] != name:
            raise SystemExit(f"alias collision: {name}, {occupied[alias]}")
        occupied[alias] = name
        dest = args.output / f"{alias}.cls"
        dest.write_bytes(payload)
        mapping.append(f"{dest.name} = {name} ({original})")
    (args.output / "classes.txt").write_text("\n".join(mapping) + "\n")
    (args.output / "main.txt").write_text(args.main + "\n")
    print(f"{len(mapping)} class files copied to {args.output}")
    for line in mapping:
        print(line)


if __name__ == "__main__":
    main()
