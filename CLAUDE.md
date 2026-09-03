# Clang Lifetime Safety Analysis

This repository is the LLVM monorepo. Work in this checkout focuses on
**Clang's intra-procedural Lifetime Safety Analysis**: the static analysis
that detects use-after-free, use-after-scope, dangling fields, return-of-stack-
address, and use-after-invalidation defects in C++.

The analysis is inspired by Polonius (Rust borrow checker) but adapted to C++:
no exclusivity rule, gradual annotation adoption, intra-procedural by default.
See `clang/docs/LifetimeSafety.rst` and the
[RFC](https://discourse.llvm.org/t/rfc-intra-procedural-lifetime-analysis-in-clang/86291).

## Build

CMake build directory: `build/` (already configured, Ninja).

Common targets (run from repo root):

```bash
ninja -C build clang                        # build the clang driver
ninja -C build clangAnalysisLifetimeSafety  # just the analysis library
ninja -C build AllClangUnitTests            # unit-test binary (gtest)
```

The driver binary lands at `build/bin/clang`. `build/compile_commands.json`
exists for clangd / IDE integration.

## Run tests

The lit tests live under `clang/test/Sema/` (filename pattern
`warn-lifetime-safety*.cpp` and `attr-lifetimebound*`). The unit tests live at
`clang/unittests/Analysis/LifetimeSafetyTest.cpp`.

```bash
# All Sema lit tests (includes lifetime safety)
ninja -C build check-clang-sema

# A single lit file
build/bin/llvm-lit -v clang/test/Sema/warn-lifetime-safety.cpp

# Run only the lifetime-safety lit tests
build/bin/llvm-lit -v clang/test/Sema/warn-lifetime-safety*.cpp \
                      clang/test/Sema/attr-lifetimebound*.*

# All Clang unit tests (includes LifetimeSafetyTest). This checkout builds the
# clang unit tests into a single `AllClangUnitTests` binary.
ninja -C build AllClangUnitTests
build/tools/clang/unittests/AllClangUnitTests

# Filter to the lifetime-safety unit tests
build/tools/clang/unittests/AllClangUnitTests --gtest_filter='LifetimeAnalysisTest.*'
```

`%clang_cc1` in lit RUN lines invokes the cc1 frontend directly — most lifetime
safety options (e.g. `-Wlifetime-safety`, `-flifetime-safety-inference`) are
cc1-style flags and need `-fsyntax-only` to skip codegen.

`-mllvm -debug-only=...` requires an **assert build** (`LLVM_ENABLE_ASSERTIONS=ON`).
Tests that use it are gated by `// REQUIRES: asserts`.

## Codebase layout (just the parts you'll touch)

| Area | Where |
|---|---|
| Analysis library (this is the focus) | `clang/lib/Analysis/LifetimeSafety/` |
| Analysis public headers | `clang/include/clang/Analysis/Analyses/LifetimeSafety/` |
| Sema integration (diagnostics, glue) | `clang/lib/Sema/AnalysisBasedWarnings.cpp`, `clang/lib/Sema/SemaLifetimeSafety.{h,cpp}` |
| Diagnostic groups (`-Wlifetime-safety-*`) | `clang/include/clang/Basic/DiagnosticGroups.td` |
| Driver/cc1 flags | `clang/include/clang/Options/Options.td` |
| LangOpts (`DebugRunLifetimeSafety`, `LifetimeSafetyMaxCFGBlocks`, `EnableLifetimeSafety{Inference,TUAnalysis}`) | `clang/include/clang/Basic/LangOptions.def` |
| Attribute defs (`lifetimebound`, `lifetime_capture_by`, `noescape`, `gsl::Owner/Pointer`) | `clang/include/clang/Basic/Attr.td` |
| Attribute user docs | `clang/include/clang/Basic/AttrDocs.td` |
| End-user analysis docs | `clang/docs/LifetimeSafety.rst` |
| Lit tests | `clang/test/Sema/warn-lifetime-safety*.cpp`, `clang/test/Sema/attr-lifetimebound*.*` |
| Test header shared by lit | `clang/test/Sema/Inputs/lifetime-analysis.h` |
| Unit tests (gtest, with `POINT(name)` test points) | `clang/unittests/Analysis/LifetimeSafetyTest.cpp` |

The analysis depends on Clang's CFG (`clang/lib/Analysis/CFG.cpp`) and runs
*before* CodeGen, late in Sema once a function body is complete (or at TU end
in TU-analysis mode).

## Analysis architecture

Single entry point: `clang::lifetimes::runLifetimeSafetyAnalysis(AC, SemaHelper, Stats, CollectStats)`
in `LifetimeSafety.cpp`. It builds a `LifetimeSafetyAnalysis` per
`AnalysisDeclContext` and calls `run()`, which executes a fixed pipeline:

```
AST + CFG
    │
    ▼
[1] FactsGenerator       ─►  Per-CFG-block list of Facts (LoanMgr, OriginMgr filled in)
    │
    ▼
[2] LoanPropagationAnalysis (forward dataflow, ImmutableMap<Origin,Set<Loan>>)
    │
    ▼
[3] LiveOriginsAnalysis     (backward dataflow, ImmutableMap<Origin,LivenessInfo>)
    │
    ▼
[4] MovedLoansAnalysis      (forward, tracks which loans were moved out)
    │
    ▼
[5] LifetimeChecker         (Checker.cpp; emits diagnostics via SemaHelper)
```

Optional steps: skip the function entirely if `Cfg.getNumBlockIDs() >
LifetimeSafetyMaxCFGBlocks`; collect missing-origin stats if requested.

### Core domain types (`Loans.h`, `Origins.h`, `Facts.h`)

- **`AccessPath`** — a borrowed storage location. Roots: `ValueDecl` (local /
  global), `MaterializeTemporaryExpr` (temporary), `CXXNewExpr` (heap),
  `PlaceholderParam` / `PlaceholderThis` (caller-scope placeholders that never
  expire intra-procedurally).
- **`Loan`** (`LoanID`) — a single borrow of an `AccessPath`, with the issuing
  expression (e.g. `&x`). Owned by `LoanManager`.
- **`Origin`** (`OriginID`) — symbolic identifier for the set of loans a
  pointer-like value can hold *at one level of indirection*. For
  `int** pp`, three origins (pp / *pp / **pp) chained as an `OriginList`.
  Owned by `OriginManager`. Origins are attached to `ValueDecl`s and `Expr`s.
- **`Fact`** — atomic lifetime event attached to a CFG block, dispatched by
  `Kind`:
  - `Issue`        — new loan (`&x`, parameter binding, etc.)
  - `Expire`       — `AccessPath` storage ends (variable goes out of scope)
  - `OriginFlow`   — origin propagated from `Src` to `Dest`; `KillDest=true`
                     replaces the destination's loan set, otherwise merges
                     (printed as `Merge`)
  - `Use`          — origin appears as an l-value/r-value (read or write)
  - `MovedOrigin`  — argument passed to an rvalue reference parameter
  - `OriginEscapes` — concrete kinds: `ReturnEscapeFact`, `FieldEscapeFact`,
                     `GlobalEscapeFact`
  - `InvalidateOrigin` — container mutation, `delete`, `std::destroy_at`
  - `KillOrigin`   — clears all loans of an origin
  - `TestPoint`    — synthetic marker emitted from
                     `void("__lifetime_test_point_<name>")` casts; used by
                     unit tests to query analysis state at named program
                     points.
  - All facts implement `dump(OS, LoanMgr, OriginMgr)`; that's what
    `-debug-only=LifetimeFacts` prints.
- **`ProgramPoint = const Fact *`** — "after" semantics in forward analysis,
  "before" semantics in backward analysis.
- **`FactManager`** — owns all `Fact`s, maps CFG blocks → fact lists, owns the
  `LoanManager` and `OriginManager`.

### Fact generation (`FactsGenerator.cpp`)

`FactsGenerator` is a `ConstStmtVisitor`. It walks each CFG block in program
order, emitting facts into `CurrentBlockFacts` then handing them to the
`FactManager`. Notable hooks:

- `handleAssignment` — emits `OriginFlow` (KillDest=true on the LHS, then a
  Merge into the assignment-expression origin so chained `a = b = c` works).
- `handleFunctionCall` — produces `IssueFact`s for parameter loans and
  `OriginFlow`s for `lifetimebound` parameter→return propagation; also
  dispatches to `handleInvalidatingCall`, `handleDestructiveCall`,
  `handleLifetimeCaptureBy`, `handleMovedArgsInCall`, `handleGSLPointerConstruction`.
- `handleLifetimeEnds` — emits `Expire` facts when a `CFGLifetimeEnds` element
  is visited; that's why the CFG is built with `AddLifetime = true`.
- `handleExitBlock` — emits `OriginEscapes` facts for live origins reaching
  function exit (return / dangling field at end of constructor / global stores).
- `handleTestPoint` — recognizes `void("__lifetime_test_point_<name>")` and
  emits `TestPointFact`.

GSL semantics are encoded by helpers in `LifetimeAnnotations.{h,cpp}`:
`isGslPointerType`, `isGslOwnerType`, `shouldTrackImplicitObjectArg`,
`isInvalidationMethod`, `isUniquePtrRelease`, `destructsFirstArg`, etc.

### Dataflow framework (`Dataflow.h`)

CRTP base `DataflowAnalysis<Derived, Lattice, Direction>` with a custom
worklist (`ForwardDataflowWorklist` / `BackwardDataflowWorklist`). Derived
classes override `getInitialState`, `join`, and `transfer(Lattice, const FooFact&)`
overloads for the facts they care about. State is recorded per `ProgramPoint`
in `PointToState`. Lattices use LLVM `ImmutableMap` / `ImmutableSet`
(persistent, structural sharing) — see `Utils.h::join` and the
`JoinKind::{Symmetric,Asymmetric}` distinction (asymmetric is a fast path when
the join has a left identity).

The three concrete dataflows hide their `Impl` behind a PImpl in the public
header:

| Analysis | Dir | Lattice | Purpose |
|---|---|---|---|
| `LoanPropagationAnalysis` | Forward | `OriginLoanMap = ImmutableMap<OriginID, ImmutableSet<LoanID>>` | "Which loans can each origin hold here?" |
| `LiveOriginsAnalysis` | Backward | `ImmutableMap<OriginID, LivenessInfo{CausingFact, Kind}>` | "Which origins are read on some/every path forward?" — `Kind` is `Must` / `Maybe` / `Dead`, used to escalate diagnostic confidence. |
| `MovedLoansAnalysis` | Forward | `ImmutableMap<LoanID, const Expr*>` | Tracks which loans were moved-from, so use-after-scope reports can attribute moves vs. plain expiry. |

### Checker (`Checker.cpp`)

`runLifetimeChecker` walks the post-order CFG view, handling each
`ExpireFact` / `InvalidateOriginFact` / `OriginEscapesFact`:

- `checkExpiry` — at expiry, intersects the live-origin set with loans whose
  `AccessPath` is the expired one. Each survivor becomes a `PendingWarning`.
  `causingFactDominatesExpiry` upgrades `Maybe` to a definite (dominated) report.
- `checkInvalidation` — like `checkExpiry`, but driven by container mutation /
  explicit destruction.
- `checkAnnotations` — when an origin escapes through return/field/global,
  inspects the loans it carries; if those came from `[[clang::noescape]]`
  parameters → `NoescapeWarningsMap`; if from un-annotated parameters →
  candidates for `[[clang::lifetimebound]]` suggestions.

Diagnostics are emitted through the `LifetimeSafetySemaHelper` virtual
interface (defined in `LifetimeSafety.h`, implemented in
`AnalysisBasedWarnings.cpp`/`SemaLifetimeSafety.{h,cpp}`). The interface
exists to **break the dependency cycle** clangSema → clangAnalysis →
clangAnalysisLifetimeSafety → clangSema; never `#include` Sema headers from
this library.

### Annotation inference & TU mode

- `-flifetime-safety-inference` — enables `inferAnnotations()` in the checker:
  attaches implicit `LifetimeBoundAttr` to parameters / `this` whose loans
  escape via return.
- `-fexperimental-lifetime-safety-tu-analysis` — switches Sema to defer the
  per-function analysis to TU end and walk functions in call-graph post-order
  so that inferred attributes from callees are visible to callers.

## Diagnostic groups (`-W…`)

Hierarchy (from `DiagnosticGroups.td`, around line 564):

```
-Wlifetime-safety-all
├── -Wlifetime-safety
│   ├── -Wlifetime-safety-permissive    (recommended starter set)
│   │   ├── -Wlifetime-safety-use-after-scope
│   │   ├── -Wlifetime-safety-use-after-free
│   │   ├── -Wlifetime-safety-return-stack-addr
│   │   ├── -Wlifetime-safety-dangling-field
│   │   └── -Wlifetime-safety-dangling-global
│   └── -Wlifetime-safety-strict        (incl. permissive + experimental)
│       ├── -Wlifetime-safety-use-after-scope-moved
│       ├── -Wlifetime-safety-return-stack-addr-moved
│       ├── -Wlifetime-safety-dangling-field-moved
│       ├── -Wlifetime-safety-dangling-global-moved
│       └── -Wlifetime-safety-invalidation
├── -Wlifetime-safety-suggestions
│   ├── -Wlifetime-safety-intra-tu-suggestions
│   └── -Wlifetime-safety-cross-tu-suggestions
└── -Wlifetime-safety-validations
    ├── -Wlifetime-safety-noescape
    ├── -Wlifetime-safety-lifetimebound-violation
    └── -Wlifetime-safety-misplaced-lifetimebound
        ├── -Wlifetime-safety-intra-tu-misplaced-lifetimebound
        └── -Wlifetime-safety-cross-tu-misplaced-lifetimebound
```

Run the analysis without diagnostics for crash/perf bisection:
`-fdebug-run-lifetime-safety` (cc1; backed by `LangOpts::DebugRunLifetimeSafety`).

## Driver / cc1 flags worth knowing

| Flag | Where it lives | What it does |
|---|---|---|
| `-Wlifetime-safety` (and subgroups above) | `DiagnosticGroups.td` | Enable warnings; also turns the analysis on in Sema |
| `-flifetime-safety-inference` / `-fno-…` | `Options.td` `lifetime_safety_inference` | Enable inference of `[[clang::lifetimebound]]` |
| `-fexperimental-lifetime-safety-tu-analysis` / `-fno-…` | `Options.td` `lifetime_safety_tu_analysis` | TU-end, post-order analysis |
| `-flifetime-safety-max-cfg-blocks=N` | `Options.td` `lifetime_safety_max_cfg_blocks` | Skip functions with > N CFG blocks (`0` = no limit). Default is set in `LangOptions.def`. |
| `-fdebug-run-lifetime-safety` / `-fno-…` | `Options.td` `debug_run_lifetime_safety` | Run analysis but suppress findings (used to surface crashes/regressions) |
| `-print-stats` | core | Triggers `clang::lifetimes::printStats(LSStats)` after all functions analyzed |

The internal `LifetimeSafetyMaxCFGBlocks` LangOpt is read in
`runLifetimeSafetyAnalysis` and short-circuits the pipeline.

## Dumping intermediate IR (debug builds only)

The analysis emits its internal state through `DEBUG_WITH_TYPE` channels.
Activate them via `-mllvm -debug-only=<channel>[,<channel>…]`. **Requires an
assertions build.** All the channels appear in
`clang/lib/Analysis/LifetimeSafety/LifetimeSafety.cpp`.

| Channel | What you get |
|---|---|
| `LifetimeFacts` | All Facts, grouped per CFG block, per analyzed function. This is the canonical "what facts did the generator produce?" dump. Used by `clang/test/Sema/warn-lifetime-safety-dataflow.cpp`. |
| `PrintCFG` | Clang's CFG dump in colored form (uses `Cfg.dump`). |
| `LiveOrigins` | `LiveOriginsAnalysis::dump`, prints the live-origin map *at every `TestPointFact`*. Pair it with `void("__lifetime_test_point_<name>");` markers in the source. |
| `LifetimeSafety` | Skip messages when a function is bailed out due to CFG size. |
| `EnableFilterByFunctionName` | Used in combination: `-debug-only=EnableFilterByFunctionName,<QualifiedFunctionName>` — first token enables the filtered code path, second token is itself a `DEBUG_WITH_TYPE` tag. The body dumps the AST (color), the CFG, and all facts but only for the matching function. |
| `<QualifiedFunctionName>` (any) | Same mechanism — every function's debug output is keyed by its qualified name, so you can also pass just the function name to filter. |

Statistics:

```bash
# Print missing-origin stats (per QualType / per StmtClass) for a TU
clang -cc1 -print-stats -Wlifetime-safety foo.cpp
```

These are accumulated across all analyzed functions into
`LifetimeSafetyStats` and printed by `printStats()` (see
`LifetimeStats.cpp`). Output looks like:

```
*** LifetimeSafety Missing Origin per QualType: (QualType : count) :
…
*** LifetimeSafety Missing Origin per StmtClassName: (StmtClassName : count) :
…
Total missing origins: N
```

Time-trace: every dataflow pass and the checker are wrapped in
`llvm::TimeTraceScope`. Add `-ftime-trace` (and optionally
`-ftime-trace-granularity=`) to get a Chrome-trace JSON.

### Test points (for inspecting state at chosen locations)

A `void("__lifetime_test_point_<name>")` cast becomes a `TestPointFact`. Used by:

- `clang/unittests/Analysis/LifetimeSafetyTest.cpp` (`#define POINT(name)
  void("__lifetime_test_point_" #name)`) — unit tests assert facts /
  liveness / loan sets at named points.
- `-debug-only=LiveOrigins` — only dumps state *at* test points.

Place markers like:

```c++
void target() {
  int x;
  int *p = &x;
  POINT(after_init);   // expands to the void-cast string above
  use(p);
}
```

### Useful FileCheck-style inspection

```bash
# Dump facts (CHECK against this in tests)
build/bin/clang -cc1 -mllvm -debug-only=LifetimeFacts -Wlifetime-safety foo.cpp 2>&1

# Dump CFG only
build/bin/clang -cc1 -mllvm -debug-only=PrintCFG -Wlifetime-safety foo.cpp 2>&1

# Dump live origins at user-placed __lifetime_test_point_… markers
build/bin/clang -cc1 -mllvm -debug-only=LiveOrigins -Wlifetime-safety foo.cpp 2>&1

# Restrict facts/CFG/AST dump to one function
build/bin/clang -cc1 -mllvm -debug-only=EnableFilterByFunctionName,my::ns::Foo \
    -Wlifetime-safety foo.cpp 2>&1
```

## Test patterns to follow

- **Diagnostic tests** (`warn-lifetime-safety*.cpp`): `// RUN: %clang_cc1
  -fsyntax-only -Wlifetime-safety[-…] -verify=expected[,labels] %s` plus
  `// expected-warning {{…}}` / `// expected-note {{…}}` comments.
- **Fact-dump tests** (`warn-lifetime-safety-dataflow.cpp`): `-mllvm
  -debug-only=LifetimeFacts | FileCheck`. Need `// REQUIRES: asserts`.
- **Stats** (`warn-lifetime-safety-missing-origin-stats.cpp`): `-print-stats
  | FileCheck`.
- **CFG-bailout** (`warn-lifetime-safety-cfg-bailout.cpp`): two RUN lines —
  one with `-flifetime-safety-max-cfg-blocks=N` to force the bailout, one
  without — and `-verify=bailout`/`-verify=nobailout` to assert the
  difference.
- **Cross-TU / fix-it** (`*-cross-tu.cpp`, `*-fixits.cpp`): use
  `split-file %s %t`, then `-fdiagnostics-parseable-fixits` or `-fixit` to
  exercise fix-it rewrites.
- **Shared test header**: `clang/test/Sema/Inputs/lifetime-analysis.h` provides
  minimal `std::vector` / `std::string` / iterator / `std::move` stand-ins.

## Style / layering rules

- The library is named `clangAnalysisLifetimeSafety`. Public API is in
  `clang/include/clang/Analysis/Analyses/LifetimeSafety/`; internals
  (`internal::` namespace) include `FactsGenerator`, `LoanManager`,
  `OriginManager`, `FactManager`, dataflow `Impl`s, etc.
- **Do not depend on Sema, AST writers, CodeGen, or Frontend** from this
  library. The CMake `LINK_LIBS` is `clangAST clangAnalysis clangBasic` —
  keep it that way. Sema interaction goes through the
  `LifetimeSafetySemaHelper` virtual interface.
- Lattices use LLVM persistent immutable collections (`ImmutableMap`,
  `ImmutableSet`); a single `LifetimeFactory` owns all `Factory` instances
  with `canonicalize=false` for speed.
- Origin / Loan IDs are typed `utils::ID<Tag>` wrappers — don't pass raw ints.
- New facts: subclass `Fact`, add a `Kind`, implement `classof`, register a
  `transfer` overload in any `DataflowAnalysis` that needs it, and update the
  `transferFact` switch in `Dataflow.h`.

## Attributes participating in the analysis (user-facing)

Defined in `clang/include/clang/Basic/Attr.td`, documented in
`clang/include/clang/Basic/AttrDocs.td`:

- `[[clang::lifetimebound]]` (`LifetimeBoundDocs`, AttrDocs.td:4575) —
  return value (or constructed object) may refer to the annotated parameter
  / `this`. Drives parameter-to-return origin flow in the analysis.
- `[[clang::lifetime_capture_by(X)]]` (`LifetimeCaptureByDocs`,
  AttrDocs.td:4641) — the named capturing entity `X` (another parameter,
  `this`, `global`, `unknown`, or a list) may refer to this argument.
  Currently statement-local; `X` is documentation, not used by the dataflow
  beyond the constructor-`this` case (then equivalent to `lifetimebound`).
- `[[clang::noescape]]` (`NoEscapeDocs`, AttrDocs.td:293) — pointer/reference
  parameter must not escape the function. Validated by
  `-Wlifetime-safety-noescape`.
- `[[gsl::Owner]]` / `[[gsl::Pointer]]` (Attr.td) — split types into
  resource owners and non-owning views. Many STL types are auto-recognized;
  see `LifetimeAnnotations.cpp::isGslOwnerType` / `isGslPointerType`.
