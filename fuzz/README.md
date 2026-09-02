# Fuzzing

`fuzz_roundtrip.c` is a [libFuzzer](https://llvm.org/docs/LibFuzzer.html) target
that writes an FST file and reads it back, checking every reader entry point
against a model built while writing.

## Building and running

The targets are not built by default and need clang:

```sh
CC=clang meson setup build-fuzz -Dfuzzing=true \
    -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-fuzz
mkdir corpus
./build-fuzz/fuzz/fuzz-roundtrip -max_len=2048 -detect_leaks=0 corpus/
```

`-max_len` matters more here than it usually does.  libFuzzer starts with a
small length limit and raises it as it goes, but in `-fork` mode every child
starts over, so the limit never grows and the corpus fills up with inputs of
twenty-odd bytes -- which is a configuration block and about three commands.
Passing it explicitly is what lets the target generate a program long enough to
have a hierarchy and a body at all.

The library is compiled with `-fsanitize=fuzzer-no-link` as well, so that
libFuzzer sees the coverage of `src/fstapi.c` and not only of the target, which
is what lets it steer.

`-detect_leaks=0` is needed because the writer and reader contexts are not the
subject here and a leak report would drown the round trip failures.

`src/fastlz.c` reads 16 bit words without caring about alignment, by design, so
an UndefinedBehaviorSanitizer build reports a misaligned load on every FASTLZ
packed file.  Those reports are noise and they arrive before anything else, so
add `-fno-sanitize=alignment` to the sanitizer arguments when they get in the
way of reading a real failure.

A crash artifact is replayed by passing it as an argument:

```sh
./build-fuzz/fuzz/fuzz-roundtrip -detect_leaks=0 crash-<hash>
```

## Reading a failure

When the model and the reader disagree the target prints the decoded command
sequence as pseudo code, followed by the raw input bytes, and then aborts.  That
sequence is meant to be copied into a test in `tests/`.

A crash inside the library itself never reaches that printout.  Set
`FST_FUZZ_TRACE=1` to echo each command to stderr as it is decoded, so that the
last lines before the sanitizer report are the ones that caused it.

## Checking what it reaches

The generator only finds what it can express, so it is worth measuring.  With
the coverage build of the library and the target linked against it by hand:

```sh
CC=clang meson setup build-cov -Dllvm_coverage=true
meson compile -C build-cov
clang -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping \
    -Isrc -Ibuild-cov fuzz/fuzz_roundtrip.c -o fz-cov -Lbuild-cov/src -lfst -lm

LLVM_PROFILE_FILE=prof/fz-%p.profraw LD_LIBRARY_PATH=build-cov/src \
    ./fz-cov -runs=0 corpus
llvm-profdata merge -sparse prof/*.profraw -o fz.profdata
llvm-cov report build-cov/src/libfst.so.1.0.0 -instr-profile=fz.profdata \
    -show-functions -sources src/fstapi.c
```

`-Dfuzzing=true` cannot be combined with `-Dllvm_coverage=true`: the ordinary
test executables in that build have no libFuzzer runtime to satisfy the
instrumentation the library carries, and fail to link.  Hence the hand linked
binary above, which replays an existing corpus rather than fuzzing.

## What the target does

The input is a tape of commands:

1. a configuration byte plus a few operand bytes: compressed hierarchy, pack
   type, parallel mode, repack on close, timescale, timezero, file type,
   optionally a comment, an environment variable and a dump size limit;
2. hierarchy commands: scopes, upscopes, variables of three kinds (bit vector,
   real, variable length string), aliases, attributes, enum table definitions
   and references;
3. body commands: time changes, value changes through both
   fstWriterEmitValueChange() and the typed emitters
   (fstWriterEmitValueChange32()/64()/Vec32()/Vec64(), whose word arrays are
   sized to the width exactly so that a read past the end is a sanitizer
   report), variable length value changes, flushes and dump activity toggles.

Everything the writer is asked to do is recorded in a model.  After
`fstWriterClose()` the file is opened and checked against it:

- the `fstReaderIterBlocks2()` callback stream, both callbacks;
- the `fstReaderIterBlocks()` callback stream, which has nowhere to put a
  variable length change and drops it, but has to report every fixed length
  one;
- that `fstReaderProcessHier()` and `fstReaderIterBlocks2()` writing the dump
  out as VCD run to completion; there is no model for the text, this is here to
  reach the code at all;
- `fstReaderGetValueFromHandleAtTime()`, sampled over the recorded times;
- the `fstReaderIterateHier()` record stream, record by record;
- the header fields, the counts and the dump activity list;
- that `fstReaderProcessHier()` keeps a facility process mask set before it.

## Modelled quirks

The model reproduces the behaviour the API actually has, so a deviation from a
quirk is a failure just like a deviation from the obvious behaviour.  They are
marked `QUIRK` in the source:

- a value change before any time change emits an implicit time change at zero;
- `fstWriterClose()` mocks up a value change at time zero for every fixed
  length handle when simulation time never advanced;
- once the dump size limit locks the writer, time changes and variable length
  value changes are dropped entirely, a fixed length value change only reaches
  the frame, and the section being built is discarded, so the reader sees a
  prefix of what was written;
- `fstReaderIterBlocks2()` reports a real as `%.16g` while
  `fstReaderGetValueFromHandleAtTime()` reports it as `r%.16g`;
- `fstReaderGetValueFromHandleAtTime()` has no variable length path and returns
  NULL for such a handle;
- `fstWriterGetFlushContextPending()` reports nothing pending while the writer
  is locked or before its first time change.

## Deliberately out of the generated command space

- handle `0`, and handles above the last one `fstWriterCreateVar()` returned.
  `fstWriterEmitValueChange()` only tests `handle <= maxhandle`, so handle `0`
  underflows into an out of bounds index.  That is API misuse rather than a
  round trip question.
- two value changes for the same handle at the same time.  The writer keeps
  both and nothing pins down the order the reader reports them in, so the model
  could not tell a reordering from a correct result.
- time changes that move backwards.  The time chain is delta encoded with
  unsigned varints, so a backwards step is not representable.
