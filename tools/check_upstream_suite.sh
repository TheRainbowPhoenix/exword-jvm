#!/usr/bin/env bash
# Strictly run the original cpjvm-rs Java programs on the C interpreter.
# Usage: bash tools/check_upstream_suite.sh /path/to/cpjvm-rs [BTPRF-output-dir]
#
# The upstream repository can stay private and local. No upstream code is
# copied into this repository or publicly uploaded by this script.
set -euo pipefail
upstream="${1:?usage: check_upstream_suite.sh /path/to/cpjvm-rs [BTPRF-output-dir]}"
if [[ ! -f "$upstream/tests/suite/Main.java" ]]; then
    echo "not a cpjvm-rs checkout: $upstream" >&2
    exit 2
fi
repo="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
out="${2:-$tmp/packed}"

mkdir -p "$tmp/classes"
javac --release 8 -d "$tmp/classes" \
    "$upstream"/tests/java/*.java "$upstream/tests/suite/Main.java"
python3 "$repo/tools/pack_classes.py" "$out" "$tmp/classes" --main Main

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -DJVM_HOST \
    -I"$repo/src" \
    "$repo/tests/host_vm.c" "$repo/src/loader.c" \
    "$repo/src/classfile.c" "$repo/src/vm.c" \
    "$repo/src/vm_exec.c" "$repo/src/vm_native.c" \
    -lm -o "$tmp/hostvm"

tests=(Hello Arith Objects Exceptions ArraysTest Strings Enums Micro Bench Gc)
passed=0
for name in "${tests[@]}"; do
    if "$tmp/hostvm" "$out" "$name" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
        if cmp -s "$tmp/$name.out" "$upstream/tests/expected/$name.txt"; then
            echo "[PASS] $name"
            passed=$((passed+1))
        else
            echo "[FAIL] $name: stdout differs"
            diff -u "$upstream/tests/expected/$name.txt" "$tmp/$name.out" | head -50 || true
        fi
    else
        echo "[FAIL] $name: C VM runtime error"
        tail -4 "$tmp/$name.err" || true
    fi
done
echo "C JVM: $passed / ${#tests[@]} original cpjvm-rs programs"
echo "Packed class files: $out"
[[ $passed -eq ${#tests[@]} ]]
