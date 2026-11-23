# EXWord SH7305 BTU hardware proof (C + SH assembly)

This is a small **real-hardware Java bytecode probe** for Casio EXWord,
built from the gint EXWord template. It ports the SH7305 BTU handoff from
[cpjvm-rs](https://github.com/TheRainbowPhoenix/cpjvm-rs) and the big-endian
SH7305 register model in [RuK](https://github.com/TheRainbowPhoenix/RuK/blob/sh4cc/ruk/jcore/btu.py).
It does **not** use RuK's SH3-DSP variant.

The EXWord app uses C plus an SH assembly trampoline, with no Rust compiler.
The D01 add-in ID is **BTPRF**, distinct from the original GINTT exit-test app.

**Status: hardware behavior on EXWord is NOT verified until tested on a device.**
Successful host compilation or simulated probes alone are not proof of a BTU.

## What the eight hardware checks do

Each check feeds **real Java bytecodes** to SH7305 BTU MMIO, enters via the
J_JP @R1 opcode (0x413B), and verifies that the trap/event handler reports
the correct exit type, Java PC, operand stack pointer and results.

| Check | Outcome |
|---|---|
| J_JP entry | Exact nop/iconst_1/breakpoint hardware self-test from cpjvm-rs -> 1 |
| Hello 6x7 | bipush 6; bipush 7; imul -> 42 (as in Hello.java) |
| locals | istore_0; iload_0; imul -> 35 |
| branch loop | iinc, backwards if_icmplt -> 45 |
| bit ops | iand and ishl -> 30 |
| 64bit add | lconst_1; lconst_1; ladd -> 2 (big-endian word layout) |
| div/zero | idiv by zero -> event R0=4, stack intact |
| SW handoff | ireturn -> software trap, stack intact |

The driver uses a 200,000-bytecode countdown, masks interrupts during bytecode
execution, then restores CPU registers and the BTU clock/control state.

## Device safety

The app does not touch BTU MMIO until you press **EXE**. Press **EXIT** to
leave without testing. It accesses 0xFF280000 (BTU) and 0xA4150038 (MSTPCR2),
and changes SR during the SH assembly trampoline. **Only run on a device
confirmed to have the SH7305-compatible memory map and privilege mode.**
If these addresses are unmapped, the first access could crash or freeze
instead of returning a clean failure. A register mismatch causes a failure
screen; a J_JP fallthrough fails the actual tests.

## Build and run

The existing [GitHub Actions workflow](.github/workflows/build.yml) builds the
EXWord fxSDK, gint-exword and this C project. Download the artifacts
**exword-btu-proof-ja** or **exword-btu-proof-cn** from a successful run.
Install the **BTPRF** package for your locale, launch it and press EXE.

For local builds, configure CMake with the EXWORD.cmake toolchain, the gint
install prefix and EXWORD_METADATA_SOURCE pointing to
[exword-xd-a8500-TEST](https://github.com/TheRainbowPhoenix/exword-xd-a8500-TEST).
The output is build-exword/btuproof.d01.

Host-only sanity check, which intentionally uses a small software model and
**does not count as hardware proof**:

    cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc \
      tests/host_probes.c src/probes.c -o /tmp/host-probes
    /tmp/host-probes

## How this relates to the exact cpjvm-rs Java tests

The upstream test suite comprises **Hello, Arith, Objects, Exceptions,
ArraysTest, Strings, Enums, Micro, Bench, Gc**. It executes Java class files
and compares stdout with the official expected text, not just individual
bytecodes.

**This proof does not yet run those ten full Java programs.** It validates
the same hardware self-test and bytecode building blocks. A complete C/C++
JVM running the original class files requires porting the class-file parser,
VM/frame interpreter, heap/GC, software bytecode fallback and built-in Java
library from cpjvm-rs. The hardware driver here is the first reusable piece.
Nothing is printed as a fake Java suite result.

Code layout: src/btu.[ch] performs MMIO initialization and cleanup;
src/btu_entry.S is the hardware J_JP trampoline; src/probes.c contains the
device bytecode fixtures; tests/host_probes.c checks fixture expectations
without needing EXWord hardware.
