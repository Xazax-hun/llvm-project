# ASCII Asteroids — a safe-programming-model field test

This is a small but non-trivial, performance-conscious terminal game written
**entirely** under Clang's lifetime-safety *safe programming model*
(`-Wlifetime-safety-soundness` enabled as **error**). Its purpose is to find out
how livable that model is when you actually write real application code in it.

See `clang/docs/LifetimeSafety.rst` (section "Safe Programming Model") for what
the model requires. In short, inside a soundness-error region:

* every borrow-carrying parameter must be annotated
  (`lifetimebound` / `noescape` / `lifetime_capture_by`);
* only a single level of indirection is allowed (no `int**`);
* user-defined types that can hold a borrow must declare ownership
  (`[[gsl::Owner]]` / `[[gsl::Pointer]]`);
* containers may not store indirections (no `std::vector<int*>`);
* no `mutable` fields, no `const_cast`;
* no exceptions (`throw` / `try` / `catch`);
* no naked `delete`/`free` of memory whose allocation the analysis didn't see
  (deallocation inside a destructor is exempt);
* no calls through function / member-function pointers.

## How the model is scoped

Each source file `#include`s its system headers first, then opens a region
with the markers from `annotations.h`:

```c++
#include "annotations.h"
LIFETIME_SAFE_START
// ... all of our code ...
LIFETIME_SAFE_END
```

`LIFETIME_SAFE_START` expands to a pragma that both **enables** the analysis and
upgrades soundness checks to errors — but only for code lexically inside the
region. Because the STL is included *outside* the region, standard-library
template bodies produce no diagnostics, and we need no global
`-Wlifetime-safety` flag. (Verified: the pragma alone is sufficient to turn the
analysis on.) Unavoidable raw-pointer FFI is carved back out with
`LIFETIME_UNSAFE_BEGIN` / `LIFETIME_UNSAFE_END`.

## Build & run

```bash
./build.sh                # builds ./asteroids at -O2, safe model as error
./asteroids               # play (arrow/WASD to turn+thrust, space to fire)
./asteroids --bench 2000  # headless: simulate 2000 frames, report updates/sec
```

Requires `build/bin/clang` (`ninja -C build clang`). On macOS the script picks
up the SDK sysroot automatically.

---

# Pain points & findings log

This is the actual deliverable. Each entry: what the safe model forced, whether
it was a true positive, false positive, or pure ergonomics, and the workaround.

## Architecture consequences (decisions forced by the model)

* **Handles, not pointers, for inter-entity references.** "No `vector<T*>`" and
  "single level of indirection" mean you cannot store `Asteroid*` in a list or
  hand out pointers into the entity arrays. Everything that would be a pointer
  becomes a `uint32_t` index / generation-tagged handle. This is the
  data-oriented layout anyway, so it costs nothing at runtime — arguably the
  model *pushed us toward* the faster design.

* **`std::span` views are returned `[[clang::lifetimebound]]` on `this`.**
  `Grid::cell()` hands back a `std::span<const uint32_t>` into its internal
  buffer, constructed as `std::span(entries_.data() + begin, count)`. Marking
  `this` lifetimebound is enough for the analysis to verify the view is bound to
  the grid (it tracks the borrow through `vector::data()` and the span ctor) and
  to flag a dangling use of it. The negative-control test confirms a dangling
  use through `SlotMap::find` is reported.

* **Custom `[[gsl::Owner]]` is backed by `std::vector`, not raw memory.**
  `SlotMap<T>` is an owner, but its storage is `std::vector`s. This is required,
  not just convenient: see the lifetimebound-validation finding below -- a
  borrow returned from a `lifetimebound` accessor can only be *verified* as bound
  to `this` when it flows through a well-modeled owner. A raw `T*` member breaks
  that chain.

## Bugs / false positives found (and fixed) during development

### `lost-loan` false positives on a raw-memory `[[gsl::Owner]]` (FIXED)

Writing `SlotMap<T>` (a `[[gsl::Owner]]` that owns a raw `T* = new T[cap]`
buffer plus parallel `uint32_t*` arrays, freed in the destructor) initially
tripped `-Wlifetime-safety-lost-loan` in two distinct spots. Minimal repro
bisected to:

* **(A) element copy-assignment through the raw pointer.** `d_[i] = value;`
  (whole-struct `operator=` on an element) fired; field-wise
  `d_[i].x = value.x;` did not.
* **(B) calling a member helper on `this`.** `if (!inrange(i)) return;` fired
  on the call to the `bool inrange() const` helper; inlining the same
  `i >= cap_` check did not.

Both were symptoms of the same gap: a `gsl::Owner` whose storage is a raw
`new[]` pointer wasn't carrying a tracked loan, so any operation that routed
through the element's `operator=` or passed `this` to another member function
"lost" the (nonexistent) borrow. A `std::vector`-backed owner did not have the
problem, because the loan flows through the well-modeled `std::vector`.

**Status: fixed in the analysis.** After the fix the raw-memory `SlotMap`
compiles clean under the soundness model with no opt-outs. (It was later
rewritten to be `std::vector`-backed for an unrelated reason -- see the
lifetimebound-validation finding next.) This is exactly the kind of latent gap a
real application shakes out that the lit tests didn't.

### A closed hole: `lifetimebound` accessors through a raw owning pointer

The original `SlotMap` owned raw `new[]` buffers and returned borrows from them:

```c++
T *data_;                                                   // raw owner
T &at(std::uint32_t i) [[clang::lifetimebound]] { return data_[i]; }
```

This compiled clean at first. After a later round of analysis fixes (the
soundness group becoming a complete superset that includes the `lifetimebound`
*validations*), recompiling reported:

```
error: could not verify that the return value can be lifetime bound to the
implicit this parameter [-Wlifetime-safety-lifetimebound-violation]
```

That is the experiment working as intended: a check that *had* been silently
passing (a hole) now fires. And it is a true positive -- intra-procedurally the
analysis cannot prove a raw pointer member's pointee is owned by `this`, so it
cannot honor the `lifetimebound` contract. The fix is to back the owner with a
verifiable owner whose element access already carries the provenance:

```c++
std::vector<T> data_;                                       // verifiable owner
T &at(std::uint32_t i) [[clang::lifetimebound]] { return data_[i]; } // verifies
```

`std::vector::operator[]` / `data()` are themselves `lifetimebound`, so the
borrow's chain to `this` is provable. **Lesson for adopters:** a custom
`gsl::Owner` that hands out borrows should be built on top of a modeled owner
(`std::vector`, etc.), not raw `new[]`/`delete[]`. This also simplified the code
(no destructor, no manual free).

## Library opt-outs (unsafe surface, localized)

Every place we step outside the model to call an unannotated library function.
The opt-outs are deliberately confined to I/O glue; **no game logic is opted
out**.

* **`terminal.cpp`** -- all the syscalls (`tcgetattr`/`tcsetattr`/`read`/
  `ioctl`/`write`/`nanosleep`) sit in per-call `LIFETIME_UNSAFE_BEGIN` /
  `LIFETIME_UNSAFE_END` regions; they take raw pointer parameters with
  no lifetime annotations. Input *parsing* stays in the model -- only the raw
  `read()` that fills a local byte buffer is opted out. The `termios` system
  struct is kept in a file-local static so it never appears as a member inside a
  safe-model region.
* **`render.cpp::present()`** -- the single `fwrite`/`fflush` to stdout.
* **`main.cpp`** -- the file is fully in the model, including `main(int argc,
  char **argv)`: the `char **argv` signature is exempt from the
  single-indirection rule (language-mandated), so argv is iterated in-model. The
  only opt-outs are un-annotated C library calls (`chrono`, `printf`/`fprintf`,
  `strcmp`, `atoi`), each wrapped behind a small annotated shim (`argIs`,
  `parseInt`, `printUsage`, `printBenchResult`, ...) whose body carries the
  localized `ignored` pragma. main's arg parsing, dispatch, and bench arithmetic
  are all checked.

That is the complete list. Everything else -- physics, collision, the grid, the
pool, splitting, scoring, the framebuffer rasterizer, the HUD, and now argument
handling -- is inside the soundness-error model.

## Ergonomic friction (true positives / library-annotation gaps)

### Iterator-pair STL overloads trip `unannotated-indirection`

`cursor_.assign(cellStart_.begin(), cellStart_.end() - 1)` errored: STL
iterators are borrows (`gsl::Pointer`), and libc++'s iterator-range APIs
(`assign(first, last)`, and the whole `<algorithm>` `(first, last)` family) carry
no `[[clang::noescape]]` / `[[clang::lifetimebound]]` annotations, so every such
call site is flagged. This is not a bug in the analysis -- it correctly cannot
prove the iterators don't escape -- it is a gap in *library* annotations.

**Workaround used:** prefer value/index-based APIs. Here, `cursor_ = cellStart_`
(whole-vector copy assignment) replaces the iterator-pair `assign`. In general,
the safe model nudges you toward index-based loops and range-`for` (which the
analysis handles) and away from raw iterator pairs until the STL ships
annotations. Worth noting for anyone adopting the model on iterator-heavy code.

### `[[clang::noescape]]` is type-affecting -> must be repeated on the definition

Annotating a parameter `[[clang::noescape]]` only on the header declaration made
the out-of-line definition a "conflicting type". The attribute has to appear on
both the declaration and the definition. Minor, but a paper cut when splitting
declarations across header/source.

### Range-`for` over a `std::span` (or any view) -- FIXED

The broad-phase scan reads `for (std::uint32_t slot : grid_.cell(...))` where
`cell()` returns `std::span<const std::uint32_t>`. This *used* to error with
`-Wlifetime-safety-multilevel-indirection`: range-for desugars to `auto&& __range
= <span>;`, and a reference-to-view looked like two levels of indirection
(reference -> span -> data).

**Status: fixed.** Range-for over a view is now accepted, so the code uses the
natural range-`for` directly -- no more index-based workaround. (Returning the
span from `cell()` in the first place also now verifies as `lifetimebound`, after
the span-construction-from-pointer fix.)

### `std::string` concatenation (`operator+=`) -- FIXED

Building the HUD the obvious way --

```c++
std::string hud = "Score ";
hud += std::to_string(world.score());
hud += "   Lives ";
```

-- *used* to error on every `+=`: both `operator+=(const char*)` (string literal
borrow into an un-annotated parameter) and `operator+=(std::string&&)` (the
`std::to_string` temporary).

**Status: fixed.** The analysis now recognizes `std::string`'s concatenation
operators (`operator+=`, free `operator+`) as copy-in -- the operand's characters
are deep-copied and do not escape -- so this idiomatic code compiles in-model.
The HUD now uses plain `std::string` concatenation; the earlier hand-written
`putText`/`putInt` shims were removed. Result-dangling (e.g. `string_view sv = a
+ b;`) is still caught.

### `mutable` ban forces honest signatures on scratch members

`Renderer::present()` reuses a `frame_` byte buffer across calls and is logically
const. The idiomatic C++ would be a `mutable std::string frame_;` so `present()
const` can refill it. The safe model bans `mutable`
(-Wlifetime-safety-const-subversion), so `present()` is non-const instead.
Arguably the model improving the code -- the mutation is real and now visible in
the signature -- but a behavior change adopters should expect on classes with
scratch/memoization members.

## Performance notes

### Did the safe model cost us anything at runtime? No.

The model is a *compile-time* analysis with no codegen impact, and -- the more
interesting question -- its rules did not force any pessimization:

* **No forced copies.** The annotations that let us pass things by reference
  (`[[clang::noescape]]` on `Grid::build`'s pool, on `Renderer::draw`'s world,
  on `putText`'s string) are exactly what *avoids* copies. The only by-value
  passing we do is for genuine value types (`Vec2`, `Input`, `Config`, POD
  entities) where a copy is cheaper than a reference anyway. The one
  defensive copy (`Asteroid a = pool.at(slot)` before `eraseAt`) is a ~24-byte
  POD on a rare code path (an asteroid actually being destroyed), not the hot
  loop.
* **Handles instead of pointers cost nothing.** Referring to entities by
  `uint32` index is the same speed as (often faster than) chasing pointers, and
  it is what the "no container of indirections" rule pushed us to. The grid
  stores indices; the pool hands out generation-tagged handles.
* **Allocation-free steady state.** The pool allocates once (fixed capacity).
  The grid's three buffers are reused every frame (`assign`/`resize`/copy keep
  capacity), so after warmup `rebuild()` does no allocation. Bullets/particles
  are `reserve`d. The only churn is occasional bullet/particle vector growth
  under heavy fire.

### Benchmark (`--bench`, Apple arm64, -O2)

Headless: fly the ship through a maintained asteroid field, measuring the
movement + grid-rebuild + broad-phase-collision pipeline.

| target asteroids | ms / frame | asteroid-updates / s |
|---:|---:|---:|
| 300  | ~0.0039 | ~80 M/s  |
| 1000 | ~0.0052 | ~194 M/s |
| 3000 | ~0.0102 | ~295 M/s |

10x the asteroids costs only ~2.6x the frame time: the uniform-grid broad phase
keeps collision near-linear instead of O(n^2), and per-asteroid cost actually
*drops* as fixed overhead amortizes. The structure the safe model nudged us into
(flat arrays, index handles, view-returning queries) is the same structure that
makes this fast.

---

# Overall verdict: how livable is the safe model?

**For application logic you control: very.** Once the data-oriented shape is in
place (flat arrays, integer handles, value types, `noescape`/`lifetimebound` on
the few real borrows), the entire simulation -- physics, a hand-written
`gsl::Owner` pool, a CSR spatial grid that hands out `lifetimebound` span views,
collision and splitting, an ASCII rasterizer -- compiles clean under
`-Wlifetime-safety-soundness` as **error**, with **zero** opt-outs in the game
logic. The negative-control test shows real dangling bugs are still caught
through our own annotated types.

**The experiment was a moving target -- in a good way.** Several rough edges hit
while writing it were then fixed in the analysis, and re-running the build
confirmed both the fixes and that nothing had been silently relying on a hole:

* raw-owner `lost-loan` false positives -- **fixed**;
* `std::string` concatenation (`operator+=` / `operator+`) -- **fixed**, HUD now
  uses plain concatenation;
* range-`for` over a `std::span` view -- **fixed**, collision loops use range-for;
* returning a `std::span` constructed from `vector::data()` as `lifetimebound` --
  **fixed**, `Grid::cell()` returns a view bound to `this`;
* and a hole *closed*: `lifetimebound` validations entered the soundness group
  and correctly rejected returning a borrow through a raw `T*` member, which
  pushed `SlotMap` onto `std::vector` backing (and simplified it);
* `main(int argc, char **argv)` -- **exempted**: `char **argv` no longer trips
  the single-indirection rule, so the whole of `main.cpp` (arg parsing included)
  now compiles in-model, with only the un-annotated C library calls behind
  annotated shims.

**Remaining friction is at the standard-library boundary.** The main one left is
iterator-pair STL APIs (`assign(first,last)`, the `<algorithm>` `(first,last)`
family) -- still un-annotated, so still flagged; we prefer value/index-based APIs
instead. The general adoption story holds: where you control the function, a
single annotation makes the pattern compile; the gaps that remain are libc++ not
yet being annotated.

**Concrete suggestions for the analysis, in priority order:**

1. **Finish annotating the standard library** (or ship a default model for it).
   String/span/lifetimebound friction has largely been addressed; iterator-pair
   APIs are the main remaining gap.
2. **Quality-of-life:** `[[clang::noescape]]` being type-affecting (must be
   repeated on out-of-line definitions) is an easy paper cut to document or
   relax.

The headline: the safe model and "write fast C++" did not fight. The same
discipline that satisfies the analysis -- no aliasing into owners you mutate, no
pointer soup, explicit ownership -- is the discipline that produces a tight,
cache-friendly, allocation-free hot loop.

The headline: the safe model and "write fast C++" did not fight. The same
discipline that satisfies the analysis -- no aliasing into owners you mutate, no
pointer soup, explicit ownership -- is the discipline that produces a tight,
cache-friendly, allocation-free hot loop.
