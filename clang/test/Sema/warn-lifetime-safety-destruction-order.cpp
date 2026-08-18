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
// Minimal owning pointer with a DELETER argument: the library calls that argument
// rather than destroying it, which is what makes it a hook. (The stub unique_ptr
// in the shared header takes no deleter.)
template <class T, class D> struct owner_with_deleter {
  ~owner_with_deleter();
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
