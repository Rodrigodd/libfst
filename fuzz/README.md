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
./build-fuzz/fuzz/fuzz-roundtrip -detect_leaks=0 corpus/
```

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

## What the target does

The input is a tape of commands:

1. a configuration byte plus a few operand bytes: compressed hierarchy, pack
   type, parallel mode, repack on close, timescale, timezero, file type,
   optionally a comment, an environment variable and a dump size limit;
2. hierarchy commands: scopes, upscopes, variables of three kinds (bit vector,
   real, variable length string), aliases, attributes, enum table definitions
   and references;
3. body commands: time changes, value changes of each kind, flushes and dump
   activity toggles.

Everything the writer is asked to do is recorded in a model.  After
`fstWriterClose()` the file is opened and checked against it:

- the `fstReaderIterBlocks2()` callback stream, both callbacks;
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
