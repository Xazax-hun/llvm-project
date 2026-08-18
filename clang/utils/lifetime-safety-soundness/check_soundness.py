#!/usr/bin/env python3
"""Differential soundness harness for Clang's lifetime safety analysis.

Every program under ``corpus/`` is a *deliberate* lifetime bug (use-after-scope,
use-after-return, use-after-free, dangling reference, ...). Under the "safe
programming model" -- the soundness warnings enabled as errors -- the analysis
must never silently miss one of these. This harness enforces that.

Two checks are performed for each corpus program:

1. **Analysis** (always): compile with the lifetime-safety soundness warnings
   and require at least one ``-Wlifetime-safety*`` diagnostic. A corpus program
   the analysis does not flag is a soundness *false negative* and fails.

2. **AddressSanitizer** (optional, when an ASan-capable compiler is available):
   build and run the program under ASan and require it to report a memory
   error. This cross-validates that the corpus program really is a bug at run
   time -- guarding the corpus against bit-rot -- and turns the harness into a
   differential bypass-hunter: a program ASan flags but the analysis does not
   is a confirmed bypass.

Each corpus file may carry directives in a leading comment:

    // DESC: one-line human description
    // FLAGS: extra compiler flags (e.g. -fcxx-exceptions)
    // EXPECT-ASAN: <substring> (the AddressSanitizer error kind to expect)

Exit status is non-zero if any corpus program is not flagged by the analysis
(or, with ASan enabled, if any confirmed bug is missed).
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")

ANALYSIS_FLAGS = [
    "-fsyntax-only",
    "-Wlifetime-safety",
    "-Wlifetime-safety-soundness",
    "-Wlifetime-safety-strict",
]
ASAN_FLAGS = [
    "-O0",
    "-g",
    "-fsanitize=address",
    "-fsanitize-address-use-after-scope",
]
ASAN_RUN_ENV = dict(os.environ, ASAN_OPTIONS="detect_stack_use_after_return=1")

LIFETIME_DIAG = re.compile(r"\[-W(error,-W)?lifetime-safety")
ASAN_REPORT = re.compile(r"(AddressSanitizer|ERROR: AddressSanitizer): ?([a-z-]*)")


class Color:
    OK = "\033[32m"
    BAD = "\033[31m"
    WARN = "\033[33m"
    DIM = "\033[2m"
    END = "\033[0m"

    @classmethod
    def off(cls):
        cls.OK = cls.BAD = cls.WARN = cls.DIM = cls.END = ""


def parse_directives(path):
    desc, flags, expect_asan = "", [], ""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("//"):
                if line:  # first non-comment, non-blank line: stop scanning
                    break
                continue
            if m := re.match(r"//\s*DESC:\s*(.*)", line):
                desc = m.group(1).strip()
            elif m := re.match(r"//\s*FLAGS:\s*(.*)", line):
                flags += m.group(1).split()
            elif m := re.match(r"//\s*EXPECT-ASAN:\s*(.*)", line):
                expect_asan = m.group(1).strip()
    return desc, flags, expect_asan


def run(cmd, **kw):
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=120, **kw
    )


def analysis_flags(args, file_flags):
    extra = list(args.std and [f"-std={args.std}"] or [])
    if args.isysroot:
        extra += ["-isysroot", args.isysroot]
    if args.stdlib:
        extra += [f"-stdlib={args.stdlib}"]
    return extra + ANALYSIS_FLAGS + file_flags + args.cflag


def check_analysis(args, path, file_flags):
    """Returns (warned: bool, output: str)."""
    cmd = [args.clang] + analysis_flags(args, file_flags) + [path]
    r = run(cmd)
    out = r.stderr + r.stdout
    # A hard compile error (not a lifetime diagnostic) means a broken corpus
    # entry; surface it as "not warned" with the output for debugging.
    warned = bool(LIFETIME_DIAG.search(out))
    return warned, out


def asan_available(args):
    if args.no_asan or not args.asan_clang:
        return False
    src = "int main(){int*p;{int x=1;p=&x;}return *p;}\n"
    with tempfile.TemporaryDirectory() as d:
        cf = os.path.join(d, "t.cpp")
        exe = os.path.join(d, "t")
        with open(cf, "w") as f:
            f.write(src)
        std = [f"-std={args.std}"] if args.std else []
        sysroot = ["-isysroot", args.isysroot] if args.isysroot else []
        stdlib = [f"-stdlib={args.stdlib}"] if args.stdlib else []
        c = run([args.asan_clang] + std + sysroot + stdlib + ASAN_FLAGS
                + [cf, "-o", exe])
        if c.returncode != 0:
            return False
        r = run([exe], env=ASAN_RUN_ENV)
        return bool(ASAN_REPORT.search(r.stderr + r.stdout))


def check_asan(args, path, file_flags):
    """Returns (confirmed: bool, kind: str, output: str)."""
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "a.out")
        std = [f"-std={args.std}"] if args.std else []
        sysroot = ["-isysroot", args.isysroot] if args.isysroot else []
        stdlib = [f"-stdlib={args.stdlib}"] if args.stdlib else []
        c = run([args.asan_clang] + std + sysroot + stdlib + ASAN_FLAGS
                + file_flags + args.cflag + [path, "-o", exe])
        if c.returncode != 0:
            return False, "", "compile failed:\n" + c.stderr
        r = run([exe], env=ASAN_RUN_ENV)
        out = r.stderr + r.stdout
        if m := ASAN_REPORT.search(out):
            return True, m.group(2) or "error", out
        return False, "", out


def evaluate(args, use_asan, path):
    """Runs both checks for one corpus program.

    Independent of every other program -- each ASan build gets its own temporary
    directory -- so these run concurrently. The work is almost entirely waiting on
    subprocesses, which release the GIL, so threads are enough and no corpus state has
    to be shared between processes.
    """
    desc, file_flags, expect = parse_directives(path)
    warned, aout = check_analysis(args, path, file_flags)
    confirmed, kind, sout = (None, "", "")
    if use_asan:
        confirmed, kind, sout = check_asan(args, path, file_flags)
    # A corpus entry states which run-time error it is supposed to produce. Checking it
    # catches an entry that still reports SOMETHING while no longer demonstrating what it
    # was written for -- this found seven entries whose declaration had gone stale, one of
    # which had moved from a stack bug to a heap one. Substring, so a coarse expectation
    # like "use-after-free" accepts "heap-use-after-free".
    #
    # The kind is only deterministic because this harness pins the ASan flags and options
    # it runs with; do not tighten it without keeping that true.
    drifted = bool(confirmed and expect and expect not in kind)
    return (os.path.basename(path), desc, warned, confirmed, kind, aout, sout,
            drifted, expect)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", default=os.environ.get("CLANG", "clang"),
                    help="clang used to run the analysis (the build under test)")
    ap.add_argument("--asan-clang", default=os.environ.get("ASAN_CLANG"),
                    help="AddressSanitizer-capable C++ compiler for cross-"
                         "validation (defaults to --clang; ASan is skipped if "
                         "unavailable)")
    ap.add_argument("--no-asan", action="store_true",
                    help="run only the analysis check (no run-time validation)")
    ap.add_argument("--std", default="c++20", help="-std= value")
    ap.add_argument("--isysroot", default=os.environ.get("SDKROOT"),
                    help="-isysroot (e.g. $(xcrun --show-sdk-path) on macOS)")
    ap.add_argument("--stdlib", help="-stdlib= value (e.g. libc++)")
    ap.add_argument("--cflag", action="append", default=[],
                    help="extra compiler flag (repeatable)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1,
                    help="corpus programs to check concurrently (default: number "
                         "of cores; use 1 for serial output while debugging)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print compiler/ASan output for failures")
    ap.add_argument("files", nargs="*",
                    help="specific corpus files (default: all of corpus/)")
    args = ap.parse_args()

    if not sys.stdout.isatty():
        Color.off()

    if args.asan_clang is None and not args.no_asan:
        args.asan_clang = args.clang
    use_asan = asan_available(args)
    if not args.no_asan and not use_asan:
        print(f"{Color.WARN}note:{Color.END} AddressSanitizer unavailable with "
              f"'{args.asan_clang}'; running analysis-only.\n")

    files = args.files or sorted(
        os.path.join(CORPUS, f) for f in os.listdir(CORPUS)
        if f.endswith(".cpp"))

    # map() yields in submission order, so the report stays byte-for-byte identical to
    # the serial one regardless of the order results actually arrive in.
    jobs = max(1, min(args.jobs, len(files)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda p: evaluate(args, use_asan, p), files))

    bypasses, uncaught, unconfirmed, drifted_kinds, passed = [], [], [], [], 0
    for (name, desc, warned, confirmed, kind, aout, sout, drifted,
         expect) in results:
        if warned:
            status, color = "CAUGHT", Color.OK
            passed += 1
            if use_asan and not confirmed:
                status, color = "CAUGHT (asan did not reproduce)", Color.DIM
            elif drifted:
                status = f"CAUGHT (asan: {kind}, expected {expect})"
                color = Color.WARN
                drifted_kinds.append(name)
        elif confirmed:
            status, color = f"BYPASS [asan: {kind}]", Color.BAD
            bypasses.append(name)
        elif use_asan:
            status, color = "UNCAUGHT (asan did not reproduce either)", Color.WARN
            unconfirmed.append(name)
        else:
            status, color = "UNCAUGHT", Color.BAD
            uncaught.append(name)

        print(f"  {color}{status:<42}{Color.END} {name}"
              + (f"  {Color.DIM}{desc}{Color.END}" if desc else ""))
        if args.verbose and status.startswith(("BYPASS", "UNCAUGHT")):
            print(Color.DIM + (aout or sout).rstrip() + Color.END)

    total = len(files)
    print(f"\n{passed}/{total} caught by the analysis.")
    if bypasses:
        print(f"{Color.BAD}{len(bypasses)} BYPASS(es) — real bugs the analysis "
              f"missed:{Color.END} {', '.join(bypasses)}")
    if uncaught:
        print(f"{Color.BAD}{len(uncaught)} not flagged "
              f"(no ASan to confirm):{Color.END} {', '.join(uncaught)}")
    if unconfirmed:
        print(f"{Color.WARN}{len(unconfirmed)} neither flagged nor reproduced "
              f"(check the corpus entry):{Color.END} {', '.join(unconfirmed)}")
    if drifted_kinds:
        print(f"{Color.WARN}{len(drifted_kinds)} reported a different run-time error "
              f"than declared (the entry no longer demonstrates what it was written "
              f"for):{Color.END} {', '.join(drifted_kinds)}")

    return 1 if (bypasses or uncaught or unconfirmed or drifted_kinds) else 0


if __name__ == "__main__":
    sys.exit(main())
