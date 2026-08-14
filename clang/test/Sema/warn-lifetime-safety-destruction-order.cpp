// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-destruction-order -verify=expected %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify=expected,soundness %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::vector;

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

// Standard library types are treated as safe: a container's destructor releases
// only what it owns, so it cannot reach an unrelated static object.
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

// A constexpr callee is exempt: it is meant to be evaluable without reference to
// program state.
constexpr int doubled(int x) { return x * 2; }
struct [[clang::destruction_order_safe]] CallsConstexpr {
  int n = 0;
  ~CallsConstexpr() { n = doubled(3); } // no-warning
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
