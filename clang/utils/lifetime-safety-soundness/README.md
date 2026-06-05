# Lifetime-safety soundness harness

A differential test harness for Clang's intra-procedural [lifetime safety
analysis](../../docs/LifetimeSafety.rst).

Every program under [`corpus/`](corpus) is a *deliberate* lifetime bug
(use-after-scope, use-after-return, use-after-free, dangling reference, ...).
Under the **safe programming model** — the `-Wlifetime-safety-soundness`
warnings enabled as errors — the analysis must never silently miss one of
them. This harness enforces that, and optionally cross-validates each corpus
program against AddressSanitizer.

For each corpus program the driver performs:

1. **Analysis** (always): compiles with `-Wlifetime-safety
   -Wlifetime-safety-soundness -Wlifetime-safety-strict` and requires at least
   one `-Wlifetime-safety*` diagnostic. A program the analysis does not flag is
   a soundness **false negative**.

2. **AddressSanitizer** (when an ASan-capable compiler is available): builds and
   runs the program under ASan and requires a memory error. This guards the
   corpus against bit-rot and makes the harness a *bypass hunter*: a program
   ASan flags but the analysis does not is a confirmed bypass.

## Running

```sh
# Analysis-only (portable, no run time needed):
./check_soundness.py --clang /path/to/build/bin/clang --no-asan

# Differential against AddressSanitizer (use a compiler whose runtime is built;
# the build under test will be used for both if it can link ASan):
./check_soundness.py --clang /path/to/build/bin/clang \
                     --asan-clang /path/to/asan-capable-clang++

# On macOS the STL examples need an SDK / libc++:
./check_soundness.py --clang build/bin/clang \
    --isysroot "$(xcrun --show-sdk-path)" --stdlib libc++
```

`--clang` defaults to `$CLANG` then `clang`; `--asan-clang` defaults to
`$ASAN_CLANG` then `--clang`. ASan is skipped automatically if the chosen
compiler cannot link the sanitizer runtime. The exit status is non-zero if any
corpus program is not flagged by the analysis (or, with ASan, if a confirmed
bug is missed).

## Adding a case

Drop a self-contained `.cpp` into `corpus/`. It should be a genuine lifetime
bug that triggers AddressSanitizer at run time (so `main` must actually *use*
the dangling value). A leading comment may carry directives:

```c++
// DESC: one-line description (shown in the report)
// FLAGS: extra compiler flags, e.g. -fcxx-exceptions
// EXPECT-ASAN: the ASan error kind, e.g. stack-use-after-scope
```

When the analysis is improved to model a previously-unmodeled construct, its
corpus entry keeps passing (it is still flagged, now perhaps more precisely).
When a *new* false negative is discovered, add a corpus entry reproducing it;
it will fail as a `BYPASS` until the analysis is fixed.
