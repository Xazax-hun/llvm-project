// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-destruction-order -verify=expected %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify=expected,soundness %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::vector;

namespace std {
// Minimal variadic stand-in: its alternatives arrive as a single template Pack,
// which is the path that has to be looked inside.
template <class... Ts> struct variant {
  variant();
  ~variant();
};
} // namespace std

volatile char sink;

// Objects of static storage duration are destroyed in reverse order of
// construction, and across translation units in no specified order. A destructor
// that reaches another such object may therefore read one already destroyed, and
// nothing at the use site distinguishes a live global from a dead one -- so this
// is addressed by construction rather than detection.
//
// A static-duration variable is allowed when its destruction cannot observe
// another: its type is trivially destructible, or it promises
// '[[clang::destruction_order_safe]]' and that promise is verified.

//===----------------------------------------------------------------------===//
// The ban: a static-duration object whose destructor is not known to be safe.
//===----------------------------------------------------------------------===//

struct Logger {
  ~Logger();
};

Logger g_logger; // expected-warning {{variable of static storage duration has type 'Logger', whose destructor is not known to be safe}}

// A static LOCAL has static duration too -- this is the Meyers singleton, the
// idiom recommended to fix the static *initialization* order problem, which
// reintroduces the *destruction* order one.
Logger &instance() {
  static Logger l; // expected-warning {{variable of static storage duration has type 'Logger'}}
  return l;
}

thread_local Logger g_tl; // expected-warning {{variable of static storage duration has type 'Logger'}}

// Reported once, at the definition -- not again at the in-class declaration.
struct Host {
  static Logger s_log;
};
Logger Host::s_log; // expected-warning {{static data member of static storage duration has type 'Logger'}}

// An `extern` declaration is destroyed in whichever TU defines it, and reported
// there.
extern Logger g_extern; // no-warning

//===----------------------------------------------------------------------===//
// Allowed without any annotation.
//===----------------------------------------------------------------------===//

int g_count = 42;              // trivially destructible
const char *g_lit = "literal"; // ditto: pointer to immortal storage
struct Pod {
  int a;
  float b;
};
Pod g_pod{1, 2}; // trivial destructor

// A standard library type is safe only to the extent that what it DESTROYS is: a
// container destroys its elements, so the promise follows the template arguments.
string g_name;
vector<int> g_nums;

// A class with no user-written destructor is safe when every base and member is.
struct Aggregate {
  Pod p;
  int n;
};
Aggregate g_agg; // no-warning

//===----------------------------------------------------------------------===//
// The promise, and its verification.
//===----------------------------------------------------------------------===//

// Truthful: the destructor touches only the object's own state.
struct [[clang::destruction_order_safe]] Counter {
  int n = 0;
  ~Counter() { n = 0; }
};
Counter g_counter; // no-warning

// Referencing a static-duration object with a non-trivial destructor is the
// hazard the promise denies.
struct [[clang::destruction_order_safe]] Reader {
  ~Reader() {
    // `g_counter` has a user-written destructor, so it is destroyed at shutdown
    // and may already be gone. A plain field read, so no borrow is formed --
    // the hazard is the reference itself, not what is done with it.
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter', an object of static storage duration with a non-trivial destructor}}
  }
};

// Referencing a trivially destructible one is fine: its storage outlives every
// destructor, so there is no ordering hazard.
struct [[clang::destruction_order_safe]] ReadsPod {
  ~ReadsPod() { sink = (char)g_count; } // no-warning
};

// The promise must extend through calls: a callee the analysis cannot check
// could reach a global itself.
void unchecked();
struct [[clang::destruction_order_safe]] Caller {
  ~Caller() {
    unchecked(); // expected-warning {{is 'destruction_order_safe' but calls 'unchecked', which is not}}
  }
};

// ...and it is satisfied by a callee that promises too.
[[clang::destruction_order_safe]] void checked();
struct [[clang::destruction_order_safe]] CallerOk {
  ~CallerOk() { checked(); } // no-warning
};

// A `consteval` callee is exempt: an immediate function never runs at shutdown.
consteval int immediate() { return 7; }
struct [[clang::destruction_order_safe]] CallsConsteval {
  int n = 0;
  ~CallsConsteval() { n = immediate(); } // no-warning
};

// A merely `constexpr` callee can be called at RUNTIME, so it is verified rather
// than trusted -- its body is necessarily available, which makes requiring an
// annotation on it busywork and exempting it outright a hole. A pure one passes.
constexpr int doubled(int x) { return x * 2; }
struct [[clang::destruction_order_safe]] CallsConstexpr {
  int n = 0;
  ~CallsConstexpr() { n = doubled(3); } // no-warning
};

// ...and one that reaches a global does not. The global access has to sit on a
// conditional path -- a constexpr function that *unconditionally* reads a global
// never produces a constant expression and is ill-formed -- but that is exactly
// the shape that is valid at compile time and dangerous at run time.
// The warning lands on the offending reference inside `leaky`; the note points at
// the call that reached it, so the chain from the destructor is visible.
constexpr int leaky(bool b) {
  return b ? g_counter.n : 0; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
}
struct [[clang::destruction_order_safe]] CallsLeakyConstexpr {
  int n = 0;
  ~CallsLeakyConstexpr() {
    n = leaky(true); // expected-note {{reached through 'leaky', which is 'constexpr' but is called here at runtime}}
  }
};

// Recursion in a verified constexpr callee terminates.
constexpr int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
struct [[clang::destruction_order_safe]] CallsRecursive {
  int n = 0;
  ~CallsRecursive() { n = fib(5); } // no-warning
};

// A manifestly constant-evaluated subexpression is computed at compile time, so
// nothing in it runs at shutdown.
struct [[clang::destruction_order_safe]] ConstantEvaluated {
  int n = 0;
  ~ConstantEvaluated() {
    static_assert(fib(6) == 8); // no-warning
  }
};

// An indirect call cannot be checked at all.
struct [[clang::destruction_order_safe]] Indirect {
  void (*fp)() = nullptr;
  ~Indirect() {
    fp(); // expected-warning {{performs an indirect call, whose target cannot be checked}} \
          // soundness-warning {{call through a function pointer}}
  }
};

// A lambda runs under the same constraints as whatever invokes it.
struct [[clang::destruction_order_safe]] WithLambda {
  ~WithLambda() {
    auto f = [] {
      sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
    };
    f();
  }
};

//===----------------------------------------------------------------------===//
// Subobjects: their destructors run too, as part of destroying the object, and
// those calls are implicit -- so the body check alone cannot see them.
//===----------------------------------------------------------------------===//

struct [[clang::destruction_order_safe]] HoldsUnsafeMember {
  Logger m; // expected-warning {{its member 'm' has type 'Logger', whose destructor is not known to be safe}}
  ~HoldsUnsafeMember() {}
};

struct [[clang::destruction_order_safe]] DerivesUnsafe
    : Logger { // expected-warning {{its base class has type 'Logger', whose destructor is not known to be safe}}
  ~DerivesUnsafe() {}
};

// Safe subobjects are fine, including standard library ones.
struct [[clang::destruction_order_safe]] HoldsSafe {
  string s;
  Counter c;
  int n;
  ~HoldsSafe() {}
};
HoldsSafe g_holds_safe; // no-warning

// The annotation is what makes a global of this type legal.
struct [[clang::destruction_order_safe]] Annotated {
  ~Annotated() {}
};
Annotated g_annotated; // no-warning

//===----------------------------------------------------------------------===//
// Standard library types are not blanket-safe.
//
// What a standard container destroys is its element, so the promise has to follow
// the template arguments -- including through a variadic Pack, which is how
// std::variant and std::tuple present them. And a type-ERASING type destroys
// something its type does not name at all, so nothing about it can be checked.
//===----------------------------------------------------------------------===//

vector<Logger> g_vec_unsafe; // expected-warning {{has type 'vector<Logger>', whose destructor is not known to be safe}}
vector<int> g_vec_safe;      // no-warning

// Nested through a Pack: variant's alternatives are a variadic argument list.
std::variant<int, Logger> g_var_unsafe; // expected-warning {{whose destructor is not known to be safe}}
std::variant<int, string> g_var_safe;   // no-warning

// A type-erasing type owns something the type does not name.
std::any g_any; // expected-warning {{has type 'std::any', whose destructor is not known to be safe}}

// Same for the owning smart pointer and the engaged-or-not container.
std::unique_ptr<Logger> g_up_unsafe;  // expected-warning {{whose destructor is not known to be safe}}
std::unique_ptr<int> g_up_safe;       // no-warning
std::optional<Logger> g_opt_unsafe;   // expected-warning {{whose destructor is not known to be safe}}
std::optional<string> g_opt_safe;     // no-warning

// An annotated CLASS TEMPLATE is not safe for every argument: the instantiation's
// member destructor runs too, and an implicit instantiation is not otherwise
// visited.
template <class T> struct [[clang::destruction_order_safe]] Box {
  T t;
  ~Box() {}
};
Box<Logger> g_box_unsafe; // expected-warning {{has type 'Box<Logger>', whose destructor is not known to be safe}}
Box<int> g_box_safe;      // no-warning
