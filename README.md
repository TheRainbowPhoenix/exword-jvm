# EXWord JVM — SH7305 BTU + C Java 8 class-file runtime

A small Java bytecode interpreter, with hardware acceleration using the **real
SH7305 Bytecode Translation Unit** on an EXWord electronic dictionary.
The implementation is **C + SH4 assembly**, using the EXWord fxSDK and gint.
No Rust cross-compiler or Java runtime on the EXWord is required.

The hardware driver and J_JP instruction entry are ported from
[cpjvm-rs](https://github.com/TheRainbowPhoenix/cpjvm-rs), informed by the
[RuK SH7305 BTU model](https://github.com/TheRainbowPhoenix/RuK/blob/sh4cc/ruk/jcore/btu.py).

## Current status — important

- **Hardware BTU probes:** 8/8 **confirmed on an EXWord by the device owner**
  (earlier standalone C/ASM probe). The 8 probes are still available via H/F1.
- **Java class files:** bytecode constant pools, fields, Java method Code and
  exception tables are parsed; classes are located from their internal JVM
  name, not their upload filename.
- **Native C VM:** basic objects, arrays, static/instance fields, method calls,
  String/StringBuilder, System.out/err printing, control flow, integer and
  long bytecodes are implemented. Hardware-safe bytecodes run through J_JP;
  opcodes involving virtual object handles fall back to the C interpreter.
- **Automated acceptance:** two freshly compiled public Java 8 smoke programs,
  BtuSmoke.java and BtuObjects.java, run on the **host C VM** and matches Java stdout byte-for-byte.
  The EXWord .d01 build and JA/CN packaging also succeed in CI.
- **NOT YET supported fully:** the ten complete cpjvm-rs Java tests. The C
  library is not as complete as the original Rust implementation: exception
  handling, GC, and many Java library/float operations still require work.
  Running an unsupported method produces an explicit failure.
- **EXWord class execution itself is not yet device-validated.** The known
  8/8 success only established the earlier BTU hardware proof.

## EXWord memory-limit fix

This EXWord target has 18.5 KiB for application static data and its
managed heap (plus a separate stack), compared with much larger host-machine
heaps. The original launcher attempted to `calloc()` a ~58 KiB class index,
then `malloc()` a 96 KiB buffer for **every** file. The hardware could not
possibly satisfy those requests, so the launcher displayed "Not enough
memory for class index".

The index now stores only filenames with a shared directory and individually
allocated JVM class names. The fixed index and VM tables are reserved in
the linker's static RAM section, sized to fit its hard limit; class file
reads allocate exactly their file size. The directory scanner no longer
allocates an auxiliary constant-pool table. CI tests 30 successive directory
scans and a real EXWord SH cross-build; the linker enforces the RAM limit.

**Temporary constraints:** at most **64 class files** in the selected folder,
**256 object handles** per VM invocation (and no GC yet). These limits cover
the original uploaded collection of 35 Java classes plus the 2 smoke-test
classes, but are not enough for the original full cpjvm-rs GC benchmark.
Real device validation is still needed.

## Installing the Java VM

Download a successful build artifact from
[GitHub Actions](https://github.com/TheRainbowPhoenix/exword-jvm/actions):
**exword-jvm-ja** or **exword-jvm-cn**. It contains the BTPRF add-in folder,
metadata, the D01, and two Java smoke-test classes.

On the EXWord:

1. Install the **BTPRF** folder, separately from the previous GINTT test.
2. Start the app. **EXE** scans for class files and opens the Java launcher.
3. Use **UP/DOWN** to choose a class with `main(String[])`, then **EXE** to run.
   The launcher starts with the class named in `main.txt`, if present.
4. **H** or **F1** runs the original eight hardware BTU probes.
5. **EXIT** returns to the selector/EXWord.

The class search tries these native EXWord directories (first one with
loadable class files wins): `\\drv0\\BTPRF`, `\\crd0\\BTPRF`,
`\\drv0\\Java`, `\\drv0\\JAVA`, `\\drv0`, `\\crd0`.
If the dictionary installer puts the D01 under a different folder, put the
class files in one of these directories. This is preliminary EXWord
filesystem integration; device validation of the actual installation path
is still needed.

## Compile your own Java programs

Write an ordinary Java 8 program, for example:

```java
public class MyProgram {
    public static void main(String[] args) {
        int n = 0;
        for (int i = 1; i <= 10; ++i) n += i;
        System.out.println("1..10 = " + n);
    }
}
```

On your PC (JDK required):

```sh
mkdir -p /tmp/my-classes
javac --release 8 -d /tmp/my-classes MyProgram.java
python3 tools/pack_classes.py /path/to/BTPRF /tmp/my-classes --main MyProgram
```

Upload the resulting `*.cls` and `main.txt` files **alongside the D01**,
within the BTPRF folder on the device (or one of the search locations above).

The JVM accepts **`.cls`, `.cla`, and `.class`** extensions, case-insensitively.
The tool uses `.cls` by default to work with strict 8.3 uploaders. If a
class name is longer than eight bytes, its short filename is generated
deterministically from its JVM class name. The compiled bytes are not
rewritten. The native scanner parses `this_class` in each classfile to recover
the **actual fully qualified Java class name**, including package and nested
class names. The optional `classes.txt` manifest maps aliases for people,
but the VM does not require it. The packer also resolves collisions where
two packages contain classes with the same short basename.

Java 8 (`--release 8`) is important: modern Java string concatenation uses
`invokedynamic`, which this early JVM does not yet support.

## Run the ORIGINAL cpjvm-rs suite (local)

With both repositories checked out on your PC:

```sh
bash tools/check_upstream_suite.sh /path/to/cpjvm-rs /path/to/BTPRF
```

This script compiles the original `cpjvm-rs/tests/java/*.java` along with
`tests/suite/Main.java`, packs all class files under 8.3 aliases into the
given BTPRF folder, then runs these exact programs on the **host C VM**:

```text
Hello Arith Objects Exceptions ArraysTest Strings Enums Micro Bench Gc
```

For each program it compares stdout to its original upstream
`tests/expected/<Name>.txt` byte for byte. It prints individual failures
and exits unsuccessfully unless **all ten** match. It will currently expose
incomplete JVM coverage; it is an acceptance test, not a claim of 10/10.

After preparing the folder, transfer the generated `*.cls` files alongside
the D01 to try the same class files on EXWord. In the launcher choose **Main**
for the combined suite or run individual tests by selecting their mains.

The upstream `cpjvm-rs` repository is private, so public GitHub Actions
intentionally does **not** fetch or publish its code or test files. The local
script uses your own checkout.

## Build

The GitHub Actions workflow builds the EXWord fxSDK, gint-exword, and the C
project, then packages the D01 and both BtuSmoke/BtuObjects class files for JA/CN EXWord locales.

Local build:

```sh
cmake -S . -B build-exword \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/fxsdk/lib/cmake/fxsdk/EXWORD.cmake \
    -DCMAKE_PREFIX_PATH=/path/to/gint-exword-install \
    -DEXWORD_METADATA_SOURCE=/path/to/exword-xd-a8500-TEST
cmake --build build-exword --parallel
```

The output is `build-exword/btuproof.d01` (D01 module `btup`, app ID BTPRF).

## Runtime layout

- `src/btu.[ch]`, `src/btu_entry.S`: the real SH7305 MMIO driver and
  `J_JP @R1` trampoline, including software trap/event handoff.
- `src/loader.[ch]`: EXWord OS filesystem through `gint_world_switch`
  and classfile discovery by `this_class`.
- `src/classfile.[ch]`: Java class-file metadata and constant pool parser.
- `src/vm.c`: class loading/linking, method tables, object handles.
- `src/vm_exec.c`: Java bytecode execution and BTU/software handoff.
- `src/vm_native.c`: explicit built-in Java library methods.
- `src/main.c`: EXWord class selector, text output, hardware self-tests.
- `tools/pack_classes.py`: 8.3 filename alias conversion.
- `tools/check_upstream_suite.sh`: the exact original ten-test gate.

The current VM represents references as 32-bit handles, not physical heap
addresses. For that reason it **does not** allow the hardware BTU to touch
object/array slots. Those bytecodes are handled in C, while trusted arithmetic,
primitive locals and branches can execute directly on the BTU. This is
correctness-first; a future address-backed heap can extend BTU acceleration.

Hardware warning: the app accesses privileged SH7305 BTU/CPG MMIO after
the user explicitly starts execution. Only run on a compatible device.
