// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-destruction-order -verify=expected %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify=expected,soundness %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::vector;

// The `std` stand-ins these tests need -- variant, function, owner_with_deleter --
// live in Inputs/lifetime-analysis.h alongside the others. They model LIBRARY types,
// and since a declaration in namespace `std` is now trusted only when the library
// wrote it, declaring them here would (correctly) make them user code and change what
// the tests are about.

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

// Binding a static-duration REFERENCE to a temporary creates a second object of static
// storage duration -- the lifetime-extended temporary -- and its type need not be the
// reference's. Judging only the declared type let `~Logger` run at shutdown unverified,
// because `TrivialBase` is trivially destructible and the walk stopped there.
struct TrivialBase {};
struct LoggerDerived : TrivialBase {
  ~LoggerDerived();
};
// expected-warning@+1 {{lifetime-extended temporary of static storage duration has type 'LoggerDerived'}}
static const TrivialBase &g_extended = LoggerDerived();
// An rvalue reference extends the same way.
// expected-warning@+1 {{lifetime-extended temporary of static storage duration has type 'LoggerDerived'}}
static TrivialBase &&g_extended_rvalue = LoggerDerived();

// Binding to a SUBOBJECT makes the two types unrelated entirely: this declares an `int`
// and creates a `LoggerHolder`.
struct LoggerHolder {
  int n = 0;
  ~LoggerHolder();
};
// expected-warning@+1 {{lifetime-extended temporary of static storage duration has type 'LoggerHolder'}}
static const int &g_extended_member = LoggerHolder().n;

// Inside a function, a static-duration temporary is the same object with the same
// hazard, and is reported once -- as an object of static storage duration, not also as a
// temporary by the body check, which defers to this exactly as it does for a static local.
[[clang::destruction_order_safe]] void arms() {
  // expected-warning@+1 {{lifetime-extended temporary of static storage duration has type 'LoggerDerived'}}
  static const TrivialBase &r = LoggerDerived();
}

// Reported once at the declaration when the DECLARED type is unsafe too, rather than
// twice: that case always worked.
struct LoggerPlain {
  ~LoggerPlain();
};
// expected-warning@+1 {{variable of static storage duration has type 'LoggerPlain'}}
static const LoggerPlain &g_extended_same = LoggerPlain();

// Negatives: an extended temporary that is trivially destructible, one that promises, and
// one whose destructor only releases its own storage.
struct TriviallyExtended : TrivialBase {
  int n = 0;
};
static const TrivialBase &g_ok_trivial = TriviallyExtended(); // no-warning
struct [[clang::destruction_order_safe]] SafeExtended : TrivialBase {
  ~SafeExtended() {}
};
static const TrivialBase &g_ok_safe = SafeExtended(); // no-warning
struct OwnsOnly : TrivialBase {
  string s;
};
static const TrivialBase &g_ok_owner = OwnsOnly(); // no-warning
// A temporary with ordinary full-expression lifetime is not a static-duration object.
static int g_from_temporary = LoggerHolder().n; // no-warning

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
// std::initializer_list does not own its elements.
//
// It is trivially destructible, so every rule that keys on the declared type ended
// its walk there. But declaring one at static storage duration makes the compiler
// synthesize a SEPARATE backing array, also of static storage duration, and it is
// that array's elements which are destroyed at shutdown -- by
// __cxx_global_array_dtor, running an arbitrary element destructor that nothing
// verified. An aggregate holding one is trivially destructible too, so the walk
// stopped before reaching the member.
//
// Every sibling wrapper owns what it holds and was already caught: vector, optional,
// unique_ptr, array, pair. This is the one that does not.
//===----------------------------------------------------------------------===//

// soundness-warning@+1 {{can hold a borrow but is annotated neither}}
std::initializer_list<Logger> g_il; // expected-warning {{whose destructor is not known to be safe}}
// soundness-warning@+1 {{can hold a borrow but is annotated neither}}
std::initializer_list<int> g_il_ok; // no destruction-order warning

// An aggregate holding one, with no annotation anywhere.
struct HoldsInitList {
  std::initializer_list<Logger> il;
};
// soundness-warning@+1 {{can hold a borrow but is annotated neither}}
HoldsInitList g_holds_il; // expected-warning {{whose destructor is not known to be safe}}

// ...and the subobject rule on an annotated type, which this also passed.
struct [[clang::destruction_order_safe]] AnnotatedHoldsInitList {
  std::initializer_list<Logger> il; // expected-warning {{its member 'il' has type 'std::initializer_list<Logger>'}}
};

// A safe element type stays clean, as does an aggregate holding one.
struct HoldsSafeInitList {
  std::initializer_list<int> il;
};
// soundness-warning@+1 {{can hold a borrow but is annotated neither}}
HoldsSafeInitList g_holds_safe_il; // no destruction-order warning

// The walk has no depth limit, and cannot have one. Unlike its callers it must descend
// PAST a trivially destructible type -- hiding an initializer_list inside one is the whole
// hazard -- so a limit that answered "safe" reopened the hole at nine wrappers, and one
// that answered "unsafe" reported every deeply nested aggregate instead. A by-value member
// graph is acyclic, so the recursion terminates on its own; a visited set keeps it linear
// on a wide one.
struct D1 {
  std::initializer_list<Logger> il;
};
struct D2 { D1 m; };
struct D3 { D2 m; };
struct D4 { D3 m; };
struct D5 { D4 m; };
struct D6 { D5 m; };
struct D7 { D6 m; };
struct D8 { D7 m; };
struct D9 { D8 m; };
struct D10 { D9 m; };
struct D11 { D10 m; };
// soundness-warning@+1 {{can hold a borrow but is annotated neither}}
D11 g_deeply_wrapped; // expected-warning {{whose destructor is not known to be safe}}

// ...and nesting that holds no initializer_list stays clean at the same depth, which is
// what a conservative limit got wrong.
struct S1 { int x = 0; };
struct S2 { S1 m; };
struct S3 { S2 m; };
struct S4 { S3 m; };
struct S5 { S4 m; };
struct S6 { S5 m; };
struct S7 { S6 m; };
struct S8 { S7 m; };
struct S9 { S8 m; };
struct S10 { S9 m; };
struct S11 { S10 m; };
S11 g_deeply_wrapped_safe; // no-warning

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
// A standard library specialization CALLS its hook arguments.
//
// Following the template arguments covers what a container DESTROYS -- its
// elements. But a container also calls its allocator, and a `unique_ptr` its
// deleter, while it is being destroyed at shutdown. Such an argument is normally
// trivially destructible, so the element recursion accepted it and arbitrary user
// code ran unverified: `vector<int, MyAlloc<int>>` was a "safe" static while
// `MyAlloc::deallocate` read an already-destroyed global.
//
// The names the library invokes on a hook are fixed by the standard, so they can
// be enumerated. Identification uses only the names no ordinary type carries --
// an ELEMENT has just its destructor called, so a type with a `find` or `length`
// member must not be dragged in by having one.
//===----------------------------------------------------------------------===//

struct BadAlloc {
  using value_type = int;
  int *allocate(unsigned long);
  void deallocate(int *, unsigned long);
};
vector<int, BadAlloc> g_vec_badalloc; // expected-warning {{whose destructor is not known to be safe}}

// Annotating the hooks is what makes it usable, and routes their bodies through
// the verifier.
struct OkAlloc {
  using value_type = int;
  [[clang::destruction_order_safe]] int *allocate(unsigned long);
  [[clang::destruction_order_safe]] void deallocate(int *, unsigned long);
};
vector<int, OkAlloc> g_vec_okalloc; // no-warning

struct LeakyAlloc {
  using value_type = int;
  [[clang::destruction_order_safe]] int *allocate(unsigned long);
  // soundness-warning@+1 {{parameter that can hold a borrow is not annotated}}
  [[clang::destruction_order_safe]] void deallocate(int *, unsigned long) {
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
  }
};
vector<int, LeakyAlloc> g_vec_leakyalloc;

// A deleter, comparator or hash is called through operator().
struct BadDeleter {
  void operator()(int *) const;
};
std::owner_with_deleter<int, BadDeleter> g_od_bad; // expected-warning {{whose destructor is not known to be safe}}

struct OkDeleter {
  [[clang::destruction_order_safe]] void operator()(int *) const;
};
std::owner_with_deleter<int, OkDeleter> g_od_ok; // no-warning

// An ELEMENT type is not a hook, even when its members share hook names: the
// library calls only its destructor, which the ordinary rules already cover.
struct Doc {
  int id;
  int find(int) const;
  int length() const;
  int compare(const Doc &) const;
  void assign(int);
  void copy();
  void move();
  int max_size() const;
};
vector<Doc> g_docs; // no-warning

// A dependent call inside an uninstantiated template pattern has no resolved
// callee yet, which is not the same as having no callee -- the instantiation is
// visited separately and resolves it. Reporting it here would flag every
// allocator template whose body forwards to a dependent expression, which is what
// annotating an allocator's hooks requires being able to do.
template <class T> struct Forwards {
  [[clang::destruction_order_safe]] void go(T *p) { ::operator delete(p); } // no-warning
};

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
// Code the Decl walk did not reach.
//
// Two places a body lives that walking FunctionDecl bodies with
// ShouldVisitTemplateInstantiations does not get to.
//===----------------------------------------------------------------------===//

// (1) A constructor's member-initializer list. It is not reachable from getBody(),
// so a marked constructor whose body is `{}` passed verification while its
// initializers read an object already destroyed at shutdown.
struct [[clang::destruction_order_safe]] MemInit {
  char v;
  [[clang::destruction_order_safe]] MemInit()
      : v((char)g_counter.n) {} // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
};

// A truthful one stays clean.
struct [[clang::destruction_order_safe]] MemInitOk {
  int v;
  [[clang::destruction_order_safe]] MemInitOk() : v(0) {} // no-warning
};

// (2) A GENERIC lambda's call operator is a function template minted inside an
// expression, so the traversal -- which reaches instantiated bodies through the
// specializations of the templates it enumerates -- never sees its instantiations,
// only the dependent pattern. Everything dependent was then invisible: `static T t`
// reopened the templated Meyers singleton, and a dependent CALL let a verified body
// reach arbitrary unverified code, since the "a lambda written here is already
// covered by this traversal" exemption is false when what is written is a pattern.
int g_generic = []<class T>() {
  static T t; // expected-warning {{variable of static storage duration has type 'Logger'}}
  return 0;
}.operator()<Logger>();

// A lambda with nothing to instantiate is the residual case. One written in a type
// alias template's `decltype` has no enclosing template whose instantiation would
// carry it, and Clang produces no instantiated closure for it -- `static T t` stays
// spelled `T` in the AST while an object of the substituted type really is created
// and destroyed. There is no concrete declaration anywhere to judge, so the pattern
// is judged, conservatively: `T` is unknown, hence not known to be safe. (The cost
// is that a harmless instantiation such as `Alias<int>` is reported too.)
template <class T> using Alias = decltype([] {
  static T t; // expected-warning {{variable of static storage duration has type 'T'}}
  return 0;
});
template <class T> int run_alias() { return Alias<T>{}(); }
int g_alias = run_alias<Logger>();

struct [[clang::destruction_order_safe]] CallsGenericLambda {
  ~CallsGenericLambda() {
    auto g = [](auto tag) {
      (void)sizeof(decltype(tag));
      unchecked(); // expected-warning {{is 'destruction_order_safe' but calls 'unchecked', which is not}}
    };
    g(0);
  }
};

// A generic lambda that reaches nothing unsafe stays clean.
struct [[clang::destruction_order_safe]] GenericLambdaOk {
  ~GenericLambdaOk() {
    auto g = [](auto x) { return (char)sizeof(x); };
    sink = g(0);
  }
};

//===----------------------------------------------------------------------===//
// Construction runs at shutdown too.
//
// Every type-level rule here asks whether a type's DESTRUCTOR is safe, so a type
// with a trivial destructor and a hazardous CONSTRUCTOR passes all of them. And a
// constructor that is implicit or defaulted cannot carry the promise -- there is no
// body to make one about -- yet it runs the constructor of every base and every
// member. Those calls appear nowhere in the body being verified, so one
// attribute-free wrapper was enough to launder arbitrary code into shutdown.
//===----------------------------------------------------------------------===//

// Trivially destructible, so no type rule constrains it; its constructor is the
// hazard.
struct Peeker {
  char c;
  Peeker() : c((char)g_counter.n) {}
};

// Written directly, this was always caught. The wrapper is what hid it.
struct [[clang::destruction_order_safe]] MakesPeekerDirectly {
  ~MakesPeekerDirectly() {
    Peeker p; // expected-warning {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    (void)p;
  }
};

// (1) An IMPLICIT default constructor runs its member's constructor.
struct WrapsPeeker {
  Peeker p;
};
struct [[clang::destruction_order_safe]] MakesWrapper {
  ~MakesWrapper() {
    WrapsPeeker w; // expected-warning {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    (void)w;
  }
};

// (2) ...and its BASE's constructor.
struct DerivesPeeker : Peeker {};
struct [[clang::destruction_order_safe]] MakesDerived {
  ~MakesDerived() {
    DerivesPeeker d; // expected-warning {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    (void)d;
  }
};

// (3) A template wrapper is the same shape.
template <class T> struct Box2 {
  T t;
};
struct [[clang::destruction_order_safe]] MakesBox {
  ~MakesBox() {
    Box2<Peeker> b; // expected-warning {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    (void)b;
  }
};

// (4) A verified constructor's IMPLICIT initializers run too. Only written ones
// were traversed, so leaving the member defaulted skipped it.
struct HasImplicitMemberInit {
  Peeker p;
  // expected-warning@+1 {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
  [[clang::destruction_order_safe]] HasImplicitMemberInit() {}
};

// (5) An annotated class with no user-declared constructor at all: the attribute
// cannot describe what the compiler generates, so the generated initializers are
// what gets checked.
struct [[clang::destruction_order_safe]] AnnotatedAggregate {
  Peeker p;
};
struct [[clang::destruction_order_safe]] MakesAnnotatedAggregate {
  ~MakesAnnotatedAggregate() {
    AnnotatedAggregate a; // expected-warning {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    (void)a;
  }
};

// A CONTAINER constructs its elements, and that call happens inside the library --
// nothing at the call site names it, so unlike a user type's own constructor it
// cannot be reported precisely. That makes it a question about the TYPE, following
// the template arguments the same way the destruction question does.
struct [[clang::destruction_order_safe]] MakesVectorOfPeeker {
  ~MakesVectorOfPeeker() {
    // expected-warning@+1 {{creates a local of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    vector<Peeker> v;
    (void)v;
  }
};

// Reached through a member, where the container is not the local's own type.
struct HoldsVectorOfPeeker {
  vector<Peeker> v;
};
struct [[clang::destruction_order_safe]] MakesHolder {
  ~MakesHolder() {
    // expected-warning@+1 {{creates a local of type 'HoldsVectorOfPeeker', whose construction runs code that is not known to be safe}}
    HoldsVectorOfPeeker h;
    (void)h;
  }
};

// Containers whose elements run no user code stay clean, including nested ones.
struct [[clang::destruction_order_safe]] MakesSafeContainers {
  ~MakesSafeContainers() {
    vector<int> a;     // no-warning
    string b;          // no-warning
    vector<string> c;  // no-warning
    vector<Pod> d;     // no-warning
    vector<Counter> e; // no-warning: Counter is annotated, so its ctor is verified
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
  }
};

// The promise covers the class's OWN constructor, which is verified against it -- but
// that verification walks the initializers and arrives at a member container's
// constructor, which is library code it trusts. So the container question still has
// to be asked of an annotated type's members, or annotating the WRAPPER would hide
// exactly what the rule exists to see.
struct [[clang::destruction_order_safe]] AnnotatedHoldsVector {
  vector<Peeker> v;
  [[clang::destruction_order_safe]] AnnotatedHoldsVector() {}
};
struct [[clang::destruction_order_safe]] MakesAnnotatedHolder {
  ~MakesAnnotatedHolder() {
    // expected-warning@+1 {{creates a local of type 'AnnotatedHoldsVector', whose construction runs code that is not known to be safe}}
    AnnotatedHoldsVector h;
    (void)h;
  }
};

// The escape hatch is not a silencer: annotating the element to clear the container
// rule routes that element's CONSTRUCTOR through the verifier, which then reports
// the hazard in its own right.
struct [[clang::destruction_order_safe]] AnnotatedButLeaky {
  char c;
  AnnotatedButLeaky()
      : c((char)g_counter.n) {} // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
};

// A wrapper whose member's constructor is safe stays clean.
struct WrapsCounter {
  Counter c;
};
struct [[clang::destruction_order_safe]] MakesSafeWrapper {
  ~MakesSafeWrapper() {
    WrapsCounter w; // no-warning
    (void)w;
  }
};

//===----------------------------------------------------------------------===//
// The container question is asked wherever an object is CREATED, not only at the
// two spellings that happened to be wired up.
//===----------------------------------------------------------------------===//

// It used to be asked at exactly two places: an automatic local's declaration, and a
// CXXBindTemporaryExpr. Each shape below creates the same `vector<Peeker>` -- running
// `Peeker::Peeker` at shutdown from inside the library -- through a route neither of
// those covers, so all of them were silent while the plain local was refused.
struct [[clang::destruction_order_safe]] MakesContainersEveryWay {
  ~MakesContainersEveryWay() {
    // The heap: no declaration and no temporary to hang the question on. The library
    // type's own constructor is trusted, so the per-constructor report is silent too.
    // expected-warning@+1 {{allocates an object of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    new vector<Peeker>;
    // Array new constructs each element the same way. `getAllocatedType` is the
    // element type, so this reports the same type as above.
    // expected-warning@+1 {{allocates an object of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    new vector<Peeker>[2];
    // A function-local static is constructed when control first reaches it -- here,
    // during shutdown. This was skipped as "the Walker's business", but the Walker
    // judges DESTRUCTION, and `vector<Peeker>`'s destruction is fine.
    // expected-warning@+1 {{creates a function-local static of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    static vector<Peeker> s;
  }
};

// A temporary is asked about at its CXXBindTemporaryExpr -- which exists only when the
// type needs destroying. `fixed_vector<Peeker, 2>` keeps its elements in its own storage,
// so with `Peeker` trivially destructible there is no such node at all, and creating one
// ran two `Peeker` constructors at shutdown with nothing to hang the question on.
struct [[clang::destruction_order_safe]] MakesTrivialTemporary {
  ~MakesTrivialTemporary() {
    // expected-warning@+1 {{creates a temporary of type 'std::fixed_vector<Peeker, 2>', whose construction runs code that is not known to be safe}}
    (void)std::fixed_vector<Peeker, 2>();
  }
};

// Negatives: the same routes with an element that runs no unverified code stay clean,
// and one object is reported once rather than once per route that describes it.
struct [[clang::destruction_order_safe]] MakesSafeEveryWay {
  ~MakesSafeEveryWay() {
    new vector<int>;                              // no-warning
    new vector<Counter>;                          // no-warning: Counter is annotated
    (void)std::fixed_vector<int, 2>();            // no-warning
    (void)std::fixed_vector<Counter, 2>();        // no-warning
  }
};

//===----------------------------------------------------------------------===//
// An inherited constructor runs the base's constructor, which its initializer list
// does not mention.
//===----------------------------------------------------------------------===//

// A `using Base::Base;` declaration exists precisely to run the base's constructor, and
// that call is modelled separately from the initializer list. The list is otherwise
// complete -- it holds the derived class's own member initializers, so `MemberPeeker`
// below is found -- which made this look covered while the one thing the declaration is
// for went unchecked.
struct PeekerBase {
  char c;
  PeekerBase(int n) : c((char)(g_counter.n + n)) {}
};
struct InheritsPeeker : PeekerBase {
  using PeekerBase::PeekerBase;
};
struct [[clang::destruction_order_safe]] MakesInherited {
  ~MakesInherited() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'PeekerBase', which is not}}
    InheritsPeeker d(1);
    sink = d.c;
  }
};

// A chain of using-declarations is followed to the constructor that has a body.
struct InheritsOnce : PeekerBase {
  using PeekerBase::PeekerBase;
};
struct InheritsTwice : InheritsOnce {
  using InheritsOnce::InheritsOnce;
};
struct [[clang::destruction_order_safe]] MakesMultilevel {
  ~MakesMultilevel() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'PeekerBase', which is not}}
    InheritsTwice d(1);
    sink = d.c;
  }
};

// From a template base, where the shadow declaration names the instantiated constructor.
template <class T> struct PeekerTemplateBase {
  char c;
  PeekerTemplateBase(T n) : c((char)(g_counter.n + (int)n)) {}
};
struct InheritsTemplate : PeekerTemplateBase<int> {
  using PeekerTemplateBase<int>::PeekerTemplateBase;
};
struct [[clang::destruction_order_safe]] MakesFromTemplateBase {
  ~MakesFromTemplateBase() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'PeekerTemplateBase', which is not}}
    InheritsTemplate d(1);
    sink = d.c;
  }
};

// Invoked as an implicit CONVERSION: no construction is spelled in the verified body at
// all, yet the same constructor runs.
[[clang::destruction_order_safe]] static char takeInherited(InheritsPeeker d) {
  return d.c;
}
struct [[clang::destruction_order_safe]] MakesByConversion {
  ~MakesByConversion() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'PeekerBase', which is not}}
    sink = takeInherited(1);
  }
};

// Through a default member initializer, reached by the enclosing class's own descent.
struct WrapsInherited {
  InheritsPeeker d{1};
};
struct [[clang::destruction_order_safe]] MakesWrappedInherited {
  ~MakesWrappedInherited() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'PeekerBase', which is not}}
    WrapsInherited w;
    sink = w.d.c;
  }
};

// The list really does cover the derived class's own members: with a base constructor that
// promises, this still reports the MEMBER's constructor, and did so before the base was
// followed at all.
struct PlainCtorBase {
  int k;
  [[clang::destruction_order_safe]] PlainCtorBase(int i) : k(i) {}
};
struct InheritsWithMember : PlainCtorBase {
  Peeker p;
  using PlainCtorBase::PlainCtorBase;
};
struct [[clang::destruction_order_safe]] MakesInheritedWithMember {
  ~MakesInheritedWithMember() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    InheritsWithMember d(1);
    sink = d.p.c;
  }
};

// Negatives: a base constructor that promises, a base whose constructor is implicit, and
// inheriting anything OTHER than a constructor.
struct SafeCtorBase {
  int k;
  [[clang::destruction_order_safe]] SafeCtorBase(int i) : k(i) {}
};
struct InheritsSafe : SafeCtorBase {
  using SafeCtorBase::SafeCtorBase;
};
struct ImplicitCtorBase {
  int k = 0;
};
struct InheritsImplicit : ImplicitCtorBase {
  using ImplicitCtorBase::ImplicitCtorBase;
};
struct MemberOpBase {
  int k = 0;
  void touch();
  MemberOpBase &operator=(const MemberOpBase &);
  void operator()();
};
struct InheritsOperators : MemberOpBase {
  using MemberOpBase::touch;
  using MemberOpBase::operator=;
  using MemberOpBase::operator();
};
struct [[clang::destruction_order_safe]] MakesSafeInherited {
  ~MakesSafeInherited() {
    InheritsSafe a(1);      // no-warning
    InheritsImplicit b;     // no-warning
    InheritsOperators c;    // no-warning
    sink = (char)(a.k + b.k + c.k);
  }
};

//===----------------------------------------------------------------------===//
// The descent memo is scoped to one construction site.
//===----------------------------------------------------------------------===//

// Descending through an implicit constructor is guarded so that a class reaching itself
// cannot loop, and so that a repeated member type is not walked twice. That guard used to
// last for the whole body, which made a construction reached from two places examined only
// at the first -- and, worse, made a suppressed visit still populate it, so a narrow
// opt-out around one construction silenced every later construction of the same type.
struct WrapsPeekerTwice {
  Peeker p;
};
struct [[clang::destruction_order_safe]] MakesTwoOfTheSameType {
  ~MakesTwoOfTheSameType() {
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    WrapsPeekerTwice first;
    // Reported here too: two sites are two locations.
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    WrapsPeekerTwice second;
    sink = (char)(first.p.c + second.p.c);
  }
};

// An opt-out region must not reach past itself. The suppressed construction records nothing,
// so the one after it is still examined.
struct [[clang::destruction_order_safe]] OptsOutOfTheFirst {
  ~OptsOutOfTheFirst() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-destruction-order"
    WrapsPeekerTwice suppressed; // no-warning: inside the opt-out
#pragma clang diagnostic pop
    // expected-warning@+1 {{is 'destruction_order_safe' but calls 'Peeker', which is not}}
    WrapsPeekerTwice reported;
    sink = (char)(suppressed.p.c + reported.p.c);
  }
};

//===----------------------------------------------------------------------===//
// A DEFAULT MEMBER INITIALIZER is code no type describes.
//===----------------------------------------------------------------------===//

// The type-level construction question enumerated constructors, bases and fields, and a
// field's TYPE says nothing about its in-class initializer -- which the generated
// constructor runs. So `struct DmiPeeker { int n = peekGlobal(); };` answered "safe" to
// every type-level rule while constructing it called `peekGlobal`, and that mattered
// exactly where the construction is invisible: inside a library container, where nothing
// is reported precisely and the element type's answer is all there is.
[[clang::destruction_order_safe]] int safeCompute();
int peekGlobal();

struct DmiPeeker {
  int n = peekGlobal();
};
struct [[clang::destruction_order_safe]] MakesDmiContainer {
  vector<DmiPeeker> m;
  ~MakesDmiContainer() {
    // expected-warning@+1 {{calls 'emplace_back<>' on an object of type 'vector<DmiPeeker>', whose construction runs code that is not known to be safe}}
    m.emplace_back();
    // expected-warning@+1 {{creates a local of type 'vector<DmiPeeker>', whose construction runs code that is not known to be safe}}
    vector<DmiPeeker> local;
    (void)local;
  }
};

// A container built BY an initializer is hidden the same way a container-typed member is:
// the descent arrives at the container's own constructor and trusts it, so the elements
// it builds appear nowhere. Reported against the enclosing type, which is what the reader
// wrote.
struct HoldsDmiContainer {
  int n = (vector<Peeker>(), 0);
};
struct [[clang::destruction_order_safe]] MakesDmiHolder {
  ~MakesDmiHolder() {
    // expected-warning@+1 {{creates a local of type 'HoldsDmiContainer', whose construction runs code that is not known to be safe}}
    HoldsDmiContainer h;
    (void)h;
  }
};

// Negatives. An initializer whose callee promises is safe, and so is every ordinary
// initializer -- a literal, an arithmetic constant, and a library type built from one.
// These are what make the rule usable: it must not charge for `std::string s = "hi";`.
struct DmiSafe {
  int n = safeCompute();
};
struct DmiOrdinary {
  int a = 0;
  int b = 41 + 1;
  string s = "hi";
  // A braced initializer list is itself an unmodeled construct under the soundness group,
  // independently of anything here; recorded so this section's silence is about the
  // destruction-order rule and not about that.
  // soundness-warning@+2 {{this expression is not modeled by lifetime safety analysis}}
  // soundness-warning@+1 {{cannot track this value here}}
  vector<int> v = {1, 2, 3};
  vector<string> names{};
};
struct [[clang::destruction_order_safe]] MakesSafeDmis {
  ~MakesSafeDmis() {
    vector<DmiSafe> a;
    a.emplace_back(); // no-warning
    vector<DmiOrdinary> b;
    b.emplace_back(); // no-warning
    // soundness-note@+1 {{in implicit default constructor for 'DmiOrdinary' first required here}}
    DmiOrdinary c; // no destruction-order warning
    (void)c;
  }
};

//===----------------------------------------------------------------------===//
// These walks have no depth limit, and cannot have one.
//===----------------------------------------------------------------------===//

// A limit is answerable in neither direction. Returning the conservative answer at the
// boundary reports deeply nested code that is perfectly safe; returning the permissive one
// reopens the hole the walk exists to close. And the graph these walks follow -- bases,
// by-value members and template arguments -- CAN cycle, so a limit was what kept them
// terminating. A visited set replaces it: a revisit carries no new information, because a
// first visit that decided the interesting answer would already have returned it.

struct DeepInt0 {
  int v = 0;
};
struct DeepInt1 { DeepInt0 a; };
struct DeepInt2 { DeepInt1 a; };
struct DeepInt3 { DeepInt2 a; };
struct DeepInt4 { DeepInt3 a; };
struct DeepInt5 { DeepInt4 a; };
struct DeepInt6 { DeepInt5 a; };
struct DeepInt7 { DeepInt6 a; };
struct DeepInt8 { DeepInt7 a; };
struct DeepInt9 { DeepInt8 a; };
struct DeepInt10 { DeepInt9 a; };
struct DeepInt11 { DeepInt10 a; };

struct DeepHaz0 {
  Peeker p;
};
struct DeepHaz1 { DeepHaz0 a; };
struct DeepHaz2 { DeepHaz1 a; };
struct DeepHaz3 { DeepHaz2 a; };
struct DeepHaz4 { DeepHaz3 a; };
struct DeepHaz5 { DeepHaz4 a; };
struct DeepHaz6 { DeepHaz5 a; };
struct DeepHaz7 { DeepHaz6 a; };
struct DeepHaz8 { DeepHaz7 a; };
struct DeepHaz9 { DeepHaz8 a; };
struct DeepHaz10 { DeepHaz9 a; };
struct DeepHaz11 { DeepHaz10 a; };

struct [[clang::destruction_order_safe]] MakesDeepTypes {
  ~MakesDeepTypes() {
    // Eleven levels of plain ints: nothing here runs any code at all.
    DeepInt11 safe; // no-warning
    // The same depth with a hazard at the bottom still reports -- there is no cliff in
    // either direction, at any depth.
    // expected-warning@+1 {{creates a local of type 'vector<DeepHaz11>', whose construction runs code that is not known to be safe}}
    vector<DeepHaz11> hazardous;
    // expected-warning@+1 {{calls 'size' on an object of type 'const std::vector<DeepHaz11>', whose construction runs code that is not known to be safe}}
    sink = (char)(sizeof(safe) + (hazardous.size() ? 1 : 0));
  }
};

// Eleven levels of library containers at static storage duration: safe, since what each
// destroys is ultimately an `int`.
vector<vector<vector<vector<vector<vector<vector<vector<vector<vector<vector<int>>>>>>>>>>> g_deep_nested; // no-warning

// A type that is a template argument of its own base closes a cycle no by-value member
// graph can. That this file still finishes analyzing is the assertion: with a depth limit
// it was the limit that stopped the walk, and removing one without a visited set would not
// terminate.
//
// The answer itself is "safe", and that is what a cycle guard should say: revisiting a type
// already being explored adds no information, and any other branch that does find unsafety
// still wins. Nothing else about this type is unsafe -- so there is nothing to report.
struct CyclicBase : vector<CyclicBase> {};
struct [[clang::destruction_order_safe]] MakesCyclic {
  ~MakesCyclic() {
    CyclicBase c; // no-warning
    sink = (char)(sizeof(c) ? 1 : 0);
  }
};

//===----------------------------------------------------------------------===//
// A library callee is trusted for where it was written, not for what it is
// parameterized by.
//===----------------------------------------------------------------------===//

// Library code is trusted because it does not name a user's globals. That says nothing
// about what it CONSTRUCTS: a container builds its elements, so `m.emplace_back()` on a
// `vector<Peeker>` runs `Peeker::Peeker` during shutdown with nothing in this body naming
// it. The container is a member, constructed long before, so no construction site in this
// body describes it either -- the question has to be asked at the call. The promise
// chains: `emplace_back` is safe only if `vector<Peeker>` is, which needs `Peeker` to be.
struct [[clang::destruction_order_safe]] CallsIntoContainer {
  vector<Peeker> m;
  ~CallsIntoContainer() {
    // expected-warning@+1 {{calls 'emplace_back<>' on an object of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    m.emplace_back();
    // expected-warning@+1 {{calls 'resize' on an object of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    m.resize(3);
  }
};

// Through a pointer or reference to the container, the same.
struct [[clang::destruction_order_safe]] CallsThroughPointer {
  vector<Peeker> m;
  ~CallsThroughPointer() {
    vector<Peeker> *p = &m;
    // expected-warning@+1 {{calls 'clear' on an object of type 'vector<Peeker>', whose construction runs code that is not known to be safe}}
    p->clear();
  }
};

// Containers whose elements run no unchecked code are called into freely -- this is
// ordinary library use inside a verified destructor and has to stay clean.
struct [[clang::destruction_order_safe]] CallsSafeContainers {
  vector<int> nums;
  string text;
  vector<Counter> counters;
  ~CallsSafeContainers() {
    nums.push_back(1); // no-warning
    nums.clear();      // no-warning
    text.clear();      // no-warning
    text.push_back(0x61);
    counters.push_back(Counter()); // no-warning: Counter is annotated
    sink = (char)nums.size();
  }
};

// The promise is asked of each CONSTRUCTOR, not of the class, so annotating just the
// constructor is enough to make the element safe to build. This is what the rule is
// really about -- a class-wide attribute could not express it.
struct PeekerWithSafeCtor {
  char c = 0;
  [[clang::destruction_order_safe]] PeekerWithSafeCtor() {}
};
struct [[clang::destruction_order_safe]] CallsSafeCtorContainer {
  vector<PeekerWithSafeCtor> m;
  ~CallsSafeCtorContainer() {
    m.emplace_back(); // no-warning
    vector<PeekerWithSafeCtor> local; // no-warning
    (void)local;
  }
};

//===----------------------------------------------------------------------===//
// ...nor for what it is HANDED.
//===----------------------------------------------------------------------===//

// `std::for_each(first, last, f)` invokes `f`, and the iterators' `operator*`,
// `operator++` and `operator!=`, all from inside the library. None of those calls appears
// in the verified body, so a NAMED callable escaped entirely -- while a lambda written in
// the same place was traversed and caught all along.
//
// Which member the library calls is not knowable here: an algorithm may copy, assign,
// compare, dereference, invoke or destroy what it is given, and which of those it does is
// a property of the library's implementation. So the whole type must be verified, and
// annotating the class is what does that.
struct NamedFunctor {
  void operator()(int) const { sink = (char)g_counter.n; }
};
struct UserIterator {
  int i;
  int operator*() const { return i; }
  void operator++() { ++i; }
  bool operator!=(UserIterator o) const { return i != o.i; }
};
struct [[clang::destruction_order_safe]] HandsFunctorToLibrary {
  ~HandsFunctorToLibrary() {
    // The iterator is reported first, being the first argument; one report per call.
    // expected-warning@+1 {{hands an object of type 'UserIterator' to 'for_each<UserIterator, NamedFunctor>'}}
    std::for_each(UserIterator{0}, UserIterator{1}, NamedFunctor{});
  }
};

// With library iterators, the functor itself is what gets named.
struct [[clang::destruction_order_safe]] HandsFunctorWithLibIterators {
  vector<int> v;
  ~HandsFunctorWithLibIterators() {
    // expected-warning@+2 {{hands an object of type 'NamedFunctor' to 'for_each<__gnu_cxx::basic_iterator<int>, NamedFunctor>'}}
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    std::for_each(v.begin(), v.end(), NamedFunctor{});
  }
};

// A TEMPLATED member is reached too. RD->methods() does not enumerate a
// FunctionTemplateDecl, so writing the hook as a template hid it from every check that
// walked the method list.
struct TemplatedFunctor {
  template <class T> void operator()(T) const { sink = (char)g_counter.n; }
};
struct [[clang::destruction_order_safe]] HandsTemplatedFunctor {
  vector<int> v;
  ~HandsTemplatedFunctor() {
    // expected-warning@+2 {{hands an object of type 'TemplatedFunctor' to 'for_each<__gnu_cxx::basic_iterator<int>, TemplatedFunctor>'}}
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    std::for_each(v.begin(), v.end(), TemplatedFunctor{});
  }
};

// A CONVERSION operator on the element type is user code the library runs when it uses
// the value, and it is not an `operator()` at all -- which is why enumerating the
// operators a library might call is the wrong shape for this rule.
struct ConvertsToInt {
  int i;
  operator int() const { return (int)g_counter.n; }
};
struct [[clang::destruction_order_safe]] HandsConvertible {
  vector<ConvertsToInt> v;
  ~HandsConvertible() {
    // Reported on the ITERATOR, argument zero, because its element type is what is
    // unsafe -- the rule is about the type handed in, not about a particular member.
    // expected-warning@+2 {{hands an object of type 'iterator' (aka '__gnu_cxx::basic_iterator<ConvertsToInt>') to 'find<__gnu_cxx::basic_iterator<ConvertsToInt>, ConvertsToInt>'}}
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    (void)std::find(v.begin(), v.end(), ConvertsToInt{0});
  }
};

// A lambda written in this body is exempt: its call operator is traversed as part of the
// body, so what it reaches is reported there instead -- which is the report below, on the
// lambda's own reference to a global rather than on its type.
struct [[clang::destruction_order_safe]] HandsLambda {
  vector<int> v;
  ~HandsLambda() {
    // expected-warning@+2 {{is 'destruction_order_safe' but references 'g_counter'}}
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    (void)std::any_of(v.begin(), v.end(), [](int) { return g_counter.n != 0; });
    // A lambda that reaches nothing is entirely clean.
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    (void)std::any_of(v.begin(), v.end(), [](int x) { return x != 0; }); // no-warning
  }
};

// Annotating the class is the escape hatch, and it covers every member.
struct [[clang::destruction_order_safe]] SafeFunctor {
  void operator()(int) const {}
};
struct [[clang::destruction_order_safe]] HandsSafeFunctor {
  vector<int> v;
  ~HandsSafeFunctor() {
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    std::for_each(v.begin(), v.end(), SafeFunctor{}); // no-warning
    // Library types, and scalars, are not user code.
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    (void)std::find(v.begin(), v.end(), 3); // no-warning
    // An argument of library type is not user code either.
    vector<string> names;
    // soundness-note@+3 {{assumed to be invalidated by this operation}}
    // soundness-warning@+2 {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    // soundness-warning@+1 2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
    (void)std::find(names.begin(), names.end(), string()); // no-warning: for destruction order
  }
};

// The class annotation promises about EVERY member, not only construction and
// destruction -- otherwise the promise granted at the call above would be unchecked. So
// each member is verified in its own right.
struct [[clang::destruction_order_safe]] PromisesAllMembers {
  ~PromisesAllMembers() {}
  void touch() {
    sink = (char)g_counter.n; // expected-warning {{'touch' is 'destruction_order_safe' but references 'g_counter'}}
  }
  // soundness-warning@+1 {{parameter that can hold a borrow is not annotated for lifetime safety}}
  int compare(const PromisesAllMembers &) const {
    return (int)g_counter.n; // expected-warning {{'compare' is 'destruction_order_safe' but references 'g_counter'}}
  }
};

//===----------------------------------------------------------------------===//
// Trust follows who WROTE the code, not the namespace it is spelled in.
//
// Specializing a standard template for a program-defined type is legal, conforming
// C++, and the specialization lands literally in namespace `std`. A test that asks
// only about the namespace therefore hands arbitrary user code the trust meant for
// the library: `template <> struct std::hash<Key> { ~hash(); };` became a type whose
// destructor nothing verifies, and the same licence reached a verified body's callees,
// a container's hooks, and a class-specific `operator delete`.
//
// So a declaration in a library namespace is trusted only when it is also in a system
// header. An implicit specialization such as `vector<int>` reports the pattern's
// location in the library header, so ordinary library use is unaffected -- the stubs
// this file includes are in a header marked `#pragma clang system_header` for exactly
// that reason.
//===----------------------------------------------------------------------===//

struct HashKey {};

namespace std {
// User-written, in namespace std: not library-owned, so the ordinary rules apply and
// this destructor is not taken on trust.
template <> struct hash<HashKey> {
  ~hash();
};
} // namespace std

std::hash<HashKey> g_user_spec; // expected-warning {{whose destructor is not known to be safe}}

// A verified body may not call a member of such a specialization either.
namespace std {
template <> struct hash<int *> {
  void poke() const;
};
} // namespace std
struct [[clang::destruction_order_safe]] CallsUserSpecMember {
  ~CallsUserSpecMember() {
    std::hash<int *> t;
    t.poke(); // expected-warning {{is 'destruction_order_safe' but calls 'poke', which is not}}
  }
};

// Ordinary library types keep their trust: their specializations are implicit, and the
// pattern lives in the (system) header.
string g_lib_string;
vector<int> g_lib_vector;
vector<string> g_lib_nested;

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
// A deallocation function is shutdown code, and is not a destructor.
//
// A class-specific `operator delete` runs when an owner of the type destroys it
// -- `std::unique_ptr<T>`'s destructor calls it -- so it is user code that runs at
// shutdown. Nothing in the destructor rules reached it: it is not the destructor,
// and a TRIVIALLY DESTRUCTIBLE type has no destructor to check, which is exactly
// the case where an owner of it still calls this function. So `Foo` below was a
// "safe" type and `std::unique_ptr<Foo>` a legal static, while arbitrary user code
// ran during static destruction.
//===----------------------------------------------------------------------===//

using size_t = decltype(sizeof(0));

// Trivially destructible, so no destructor runs -- but this does.
struct FooDealloc {
  int v;
  static void operator delete(void *, size_t);
};

// Banned as a static, because destroying the unique_ptr calls the above.
std::unique_ptr<FooDealloc> g_up_dealloc; // expected-warning {{whose destructor is not known to be safe}}
// A member of that type is the same situation, reported by the subobject walk.
struct [[clang::destruction_order_safe]] HoldsDealloc {
  FooDealloc f; // expected-warning {{its member 'f' has type 'FooDealloc'}}
  ~HoldsDealloc() {}
};

// The array form is the same.
struct BarDealloc {
  int v;
  static void operator delete[](void *, size_t);
};
std::unique_ptr<BarDealloc[]> g_up_dealloc_arr; // expected-warning {{whose destructor is not known to be safe}}

// Annotating the deallocation function is what makes the type usable again -- and
// puts that function's body through the same verifier. (A `void *` parameter is
// opaque, so it draws -Wlifetime-safety-unannotated-indirection under the full
// soundness set -- unrelated to destruction order.)
struct SafeDealloc {
  int v;
  // soundness-warning@+1 {{parameter that can hold a borrow is not annotated}}
  [[clang::destruction_order_safe]] static void operator delete(void *, size_t) {}
};
std::unique_ptr<SafeDealloc> g_up_safe_dealloc; // no-warning

struct LeakyDealloc {
  int v;
  // soundness-warning@+1 {{parameter that can hold a borrow is not annotated}}
  [[clang::destruction_order_safe]] static void operator delete(void *, size_t) {
    sink = (char)g_counter.n; // expected-warning {{is 'destruction_order_safe' but references 'g_counter'}}
  }
};

// An inherited one is found by the same lookup `delete` performs.
struct DerivesDealloc : FooDealloc {};
std::unique_ptr<DerivesDealloc> g_up_derived_dealloc; // expected-warning {{whose destructor is not known to be safe}}

// A user-REPLACED global deallocation function runs whenever anything is freed,
// including while static objects are being destroyed, and there is no type at
// fault to ban -- so like '__attribute__((destructor))' it is verified on sight,
// with no annotation required. (The implementation's own, declared implicitly or
// by <new>, is exempt: it cannot reach a user's globals, and an annotated
// class-specific `operator delete` has to be able to forward to it.)
// soundness-warning@+1 {{parameter that can hold a borrow is not annotated}}
void operator delete(void *p) noexcept {
  sink = (char)g_counter.n; // expected-warning {{'operator delete' runs during static destruction but references 'g_counter'}}
}

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
