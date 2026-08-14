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
// Minimal type-erasing callable: the target it dispatches to appears nowhere in
// the type, which is what makes invoking it an indirect call.
template <class Sig> struct function {
  function();
  template <class F> function(F);
  ~function();
  void operator()() const;
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

// `constexpr` is NOT an exemption. Called in a constant-evaluated context it can
// do no harm, and such contexts are skipped wholesale (see below). Called at
// RUNTIME it is an ordinary call and follows the ordinary rule: the callee must
// carry the promise. Following constexpr bodies instead was tried and dropped --
// it made the rule depend on what happened to be visible, and reported the same
// helper once per caller.
constexpr int doubled(int x) { return x * 2; }
struct [[clang::destruction_order_safe]] CallsConstexpr {
  int n = 0;
  ~CallsConstexpr() {
    n = doubled(3); // expected-warning {{calls 'doubled', which is not}}
  }
};

// Annotating it is what makes the call legal -- and puts its own body through the
// same check.
[[clang::destruction_order_safe]] constexpr int tripled(int x) { return x * 3; }
struct [[clang::destruction_order_safe]] CallsAnnotatedConstexpr {
  int n = 0;
  ~CallsAnnotatedConstexpr() { n = tripled(3); } // no-warning
};

// ...and once annotated, a constexpr function that reaches a global is reported in
// its own right. (The access has to sit on a conditional path: one that
// *unconditionally* reads a global never produces a constant expression and is
// ill-formed -- yet this shape is valid at compile time and dangerous at runtime.)
[[clang::destruction_order_safe]] constexpr int leaky(bool b) {
  return b ? g_counter.n : 0; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
}

// A manifestly constant-evaluated subexpression is computed at compile time, so
// nothing in it runs at shutdown -- and no annotation is needed on the callee.
constexpr int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
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

// A member function pointer is the same situation.
struct Callee2 {
  void go();
};
struct [[clang::destruction_order_safe]] IndirectMember {
  void (Callee2::*pmf)() = nullptr;
  ~IndirectMember() {
    Callee2 c;
    (c.*pmf)(); // expected-warning {{performs an indirect call, whose target cannot be checked}} \
                // soundness-warning {{call through a function pointer}}
  }
};

// Invoking a TYPE-ERASING callable is an indirect call wearing a direct call's
// clothes: it resolves to a real `operator()` in namespace std, which would
// otherwise be trusted, while the target appears nowhere in the type. Reached
// through a POINTER member here, since a `std::function` member or local is
// already refused as a type whose destruction is unchecked.
struct [[clang::destruction_order_safe]] InvokesErased {
  // soundness-warning@+1 {{uses more than one level of indirection}}
  std::function<void()> *pf = nullptr; // pointer: trivially destructible
  ~InvokesErased() {
    (*pf)(); // expected-warning {{performs an indirect call, whose target cannot be checked}}
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

// An annotated CLASS TEMPLATE is not safe for every argument. Two diagnostics:
// the instantiated member is reported directly (instantiations are visited, which
// is what makes `static T t;` in a template pattern checkable at all), and the
// global of that type is banned.
template <class T> struct [[clang::destruction_order_safe]] Box {
  T t; // expected-warning {{its member 't' has type 'Logger', whose destructor is not known to be safe}}
  ~Box() {}
};
Box<Logger> g_box_unsafe; // expected-warning {{has type 'Box<Logger>', whose destructor is not known to be safe}}
Box<int> g_box_safe;      // no-warning

//===----------------------------------------------------------------------===//
// Template instantiations are visited.
//
// A dependent pattern says nothing about the hazard: `static T t;` is harmless
// until T is known. Checking only the pattern let an arbitrary unsafe type
// acquire static storage duration with no annotation anywhere -- the templated
// Meyers singleton, which is a very common idiom.
//===----------------------------------------------------------------------===//

template <class T> T &singleton() {
  static T t; // expected-warning {{variable of static storage duration has type 'Logger'}}
  return t;
}
int g_force = (singleton<Logger>(), 0);

// The same through a member function of a class template.
template <class T> struct Holder2 {
  static T &get() {
    static T t; // expected-warning {{variable of static storage duration has type 'Logger'}}
    return t;
  }
};
int g_force2 = (Holder2<Logger>::get(), 0);

// Instantiating with a safe type stays clean.
int g_force_ok = (singleton<int>(), 0);

//===----------------------------------------------------------------------===//
// The promise must be repeated on overrides.
//
// The callee check resolves against the statically named method, so an override
// that drops the promise is never verified and runs unchecked at shutdown. Every
// sibling attribute already enforces this.
//===----------------------------------------------------------------------===//

// The promise sits on the METHOD here, so this section is only about a
// non-destructor virtual; the destructor case is below, where the class-level
// spelling matters.
struct Ticker {
  // expected-note@+1 {{overridden virtual function is here}}
  [[clang::destruction_order_safe]] virtual void tick() {}
  virtual ~Ticker() {}
};

struct DropsPromise : Ticker {
  // expected-warning@+1 {{does not carry that promise}}
  void tick() override { sink = (char)g_counter.n; }
};

// Repeating it routes the override's own body through the verifier.
struct KeepsPromise : Ticker {
  [[clang::destruction_order_safe]] void tick() override {
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
  }
};

// A truthful override that repeats the promise is clean.
struct HonestOverride : Ticker {
  [[clang::destruction_order_safe]] void tick() override {}
};

// The promise is usually written on the CLASS, where it is a promise about that
// class's destructor -- so the two spellings have to answer "does this carry the
// promise?" the same way. Reading only the attribute on the destructor
// declaration meant the class-level form (the one the documentation shows) made
// the type legal to hold static storage duration, and legal to `delete`, while
// never requiring a derived destructor to promise anything. `~Derived` then ran
// unverified at shutdown -- and the `delete` check cannot cover it, since it can
// only judge the static type while dispatch picks the dynamic one.
struct [[clang::destruction_order_safe]] ClassPromise {
  // expected-note@+1 2 {{overridden virtual function is here}}
  virtual ~ClassPromise() = default;
};

struct DtorDropsClassPromise : ClassPromise {
  // expected-warning@+1 {{does not carry that promise}}
  ~DtorDropsClassPromise() override { sink = (char)g_counter.n; }
};

// An IMPLICIT destructor is an override too, and is reported at the class.
struct HasUnsafeMember2 {
  Logger m;
};
// expected-warning@+2 {{does not carry that promise}}
// expected-note@+1 {{while declaring the implicit destructor for 'ImplicitDtorDropsPromise'}}
struct ImplicitDtorDropsPromise : ClassPromise {
  HasUnsafeMember2 m; // its destructor runs unverified at shutdown
};

// Either spelling satisfies the rule, and routes the override's body through the
// verifier -- which then reports the untruth.
struct [[clang::destruction_order_safe]] KeepsViaClass : ClassPromise {
  ~KeepsViaClass() override {
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
  }
};

struct KeepsViaDtor : ClassPromise {
  [[clang::destruction_order_safe]] ~KeepsViaDtor() override {
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
  }
};

// A truthful derived class, and one whose destructor is implicit, are clean.
struct [[clang::destruction_order_safe]] HonestDerived : ClassPromise {
  int n = 0;
  ~HonestDerived() override { n = 0; }
};
struct [[clang::destruction_order_safe]] HonestImplicitDerived : ClassPromise {};

//===----------------------------------------------------------------------===//
// A borrow CAPTURED by a static-duration initializer.
//
// The verifier asks what names a destructor body mentions, so storing a reference
// to another static object launders the victim's identity: `~Reader` looks like it
// only touches `this`. The capture itself is the reportable event, and it happens
// in the initializer -- which is a constant expression here, since binding a
// reference to a global is constant-evaluable, so it was skipped as "cannot create
// a dangling borrow". Binding a reference is exactly how one is created.
//===----------------------------------------------------------------------===//

struct [[clang::destruction_order_safe]] Held {
  string s;
  // The class attribute is a promise about destruction, not a warrant for every
  // method, so an accessor called from a verified body carries its own.
  [[clang::destruction_order_safe]] char read() const { return *s.data(); }
};
extern Held g_held;

struct [[gsl::Pointer]] [[clang::destruction_order_safe]] Watcher {
  Held &m;
  ~Watcher() { sink = m.read(); }
};
// Reported at the variable whose initializer captured the borrow. The diagnostic
// is -Wlifetime-safety-view-on-mutable-global, which is in the full soundness set
// rather than the narrow destruction-order group.
Watcher g_watcher{g_held}; // soundness-warning {{borrows from a mutable global or static object}}
Held g_held{};

// Negative: a static-duration variable capturing nothing stays clean, and a
// constant initializer that cannot hold a borrow is not even analyzed.
struct [[clang::destruction_order_safe]] Plain2 {
  int n = 0;
  ~Plain2() {}
};
Plain2 g_plain2;
constexpr int g_const2 = 7;
const char *g_lit2 = "literal"; // points at immortal storage

//===----------------------------------------------------------------------===//
// Destroying an object is never a `CallExpr`.
//
// There is no AST node for the implicit destruction of a local, a temporary, or
// the target of `delete`, so a body that creates one runs that destructor with
// nothing checking it. The type has to be tested directly, as the record-level
// walk already does for bases and members.
//===----------------------------------------------------------------------===//

struct Peek {
  ~Peek(); // unannotated: not known to be safe
};

struct [[clang::destruction_order_safe]] MakesLocal {
  ~MakesLocal() {
    Peek p; // expected-warning {{creates a local of type 'Peek', whose destructor is not known to be safe}}
    (void)p;
  }
};

struct [[clang::destruction_order_safe]] MakesTemporary {
  ~MakesTemporary() {
    Peek(); // expected-warning {{creates a temporary of type 'Peek', whose destructor is not known to be safe}}
  }
};

struct [[clang::destruction_order_safe]] Deletes {
  Peek *p;
  ~Deletes() {
    delete p; // expected-warning {{destroys an object of type 'Peek', whose destructor is not known to be safe}}
  }
};

// A local of a safe type is fine.
struct [[clang::destruction_order_safe]] MakesSafeLocal {
  ~MakesSafeLocal() {
    Counter c; // no-warning
    (void)c;
    string s; // no-warning: standard library
    (void)s;
  }
};

//===----------------------------------------------------------------------===//
// Implicit code runs at shutdown too.
//===----------------------------------------------------------------------===//

// A default argument is evaluated at the CALL, so it can name a global even
// though nothing in the written body does.
[[clang::destruction_order_safe]] void consume(char c = (char)g_counter.n); // expected-warning {{references 'g_counter'}}
struct [[clang::destruction_order_safe]] UsesDefaultArg {
  ~UsesDefaultArg() { consume(); }
};

// An NSDMI runs when the member is constructed, and names whatever it likes --
// so an implicit constructor is not inert.
struct Snoop {
  char c = (char)g_counter.n; // expected-warning {{references 'g_counter'}}
};
struct [[clang::destruction_order_safe]] UsesNSDMI {
  ~UsesNSDMI() {
    Snoop s;
    sink = s.c;
  }
};

//===----------------------------------------------------------------------===//
// Shutdown code that is not a destructor at all.
//===----------------------------------------------------------------------===//

// '__attribute__((destructor))' runs during shutdown without being the destructor
// of anything, so the variable-level rule never reaches it. Declaring it a
// shutdown handler IS the declaration that it runs then, so no annotation is
// needed to subject it to the rules -- and the diagnostic says so, rather than
// claiming the function carries a promise it does not.
__attribute__((destructor(101))) static void late() {
  sink = (char)g_counter.n; // expected-warning {{'late' runs during static destruction but references 'g_counter'}}
}

__attribute__((destructor(101))) static void late_ok() { sink = 0; } // no-warning

//===----------------------------------------------------------------------===//
// A lambda declared outside the verified body.
//===----------------------------------------------------------------------===//

// A lambda written inside the body is covered by the traversal, but one declared
// at namespace scope is a separate function and follows the ordinary callee rule.
constexpr auto peek_global = [](bool b) { return b ? (char)g_counter.n : 'x'; };
struct [[clang::destruction_order_safe]] CallsOuterLambda {
  ~CallsOuterLambda() {
    sink = peek_global(true); // expected-warning {{calls 'operator()', which is not}}
  }
};
