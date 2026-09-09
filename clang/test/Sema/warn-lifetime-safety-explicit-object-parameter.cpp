// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-unused -verify %s

// A C++23 explicit object parameter is a REAL parameter: for `f(this V self)`
// the object argument binds to getParamDecl(0), and it can be taken BY VALUE, in
// which case the argument is a prvalue copy with a single origin.
//
// The call handling treated argument 0 of any instance method as an implicit
// object argument -- a glvalue whose lvalue outer origin wraps the object -- and
// read its pointee child to see through to the object. On a by-value explicit
// object parameter there is no such wrapper, and it asserted:
//
//   ArgNode->getLength() >= 2 && "Object arg of pointer type should have at
//   least two origins"
//
// The convention was already spelled out one lambda higher, where lifetimebound
// is read from getParamDecl(I) for these functions; this just follows it.

volatile int sink;

struct [[gsl::Pointer(int)]] V {
  const int *p;

  // The crashing shape: by value.
  const int *by_value(this V self [[clang::lifetimebound]]) { return self.p; }
  // By reference, which has the wrapper and did not crash -- both must work.
  // expected-warning@+2 {{uses more than one level of indirection}}
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to 'self'}}
  const int *by_ref(this const V &self [[clang::lifetimebound]]) { return self.p; }
  // An ordinary instance method, for contrast.
  // The same unverifiable-return report an ordinary accessor of this shape draws,
  // which is what shows it is not about the explicit object parameter.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to the implicit this}}
  const int *normal() const [[clang::lifetimebound]] { return p; }
};

//===----------------------------------------------------------------------===//
// No crash, and the analysis still sees through each accessor.
//===----------------------------------------------------------------------===//

void just_call(V v) { // expected-warning {{parameter that can hold a borrow is not annotated}}
  v.by_value();
  v.by_ref();
  v.normal();
}

// A borrow of a local escaping through the by-value accessor is still reported,
// and still attributed to the local rather than lost.
const int *escapes_by_value() {
  int x = 1;
  V v{&x}; // expected-warning {{stack memory associated with local variable 'x' is returned}}
  return v.by_value(); // expected-note {{returned here}}
}

// ...and through the by-reference one. The object is two levels there, so the
// borrow widens to `v` itself; reported either way.
const int *escapes_by_ref() {
  int x = 1;
  V v{&x};
  // expected-warning@+3 {{address of stack memory associated with local variable 'v' returned}}
  // expected-warning@+2 {{stack memory associated with local variable 'v' is returned}}
  // expected-note@+1 {{returned here}}
  return v.by_ref();
}

// The use-after-scope flavour through an explicit object parameter.
void use_after_scope() {
  const int *q;
  {
    int x = 1;
    V v{&x};
    q = v.by_ref(); // expected-warning {{local variable 'v' does not live long enough}}
  }                 // expected-note {{destroyed here}}
  sink = *q;        // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: nothing dangles.
//===----------------------------------------------------------------------===//

static const int kImmortal = 7;

const int *from_immortal() {
  V v{&kImmortal};
  return v.by_value(); // no-warning
}

//===----------------------------------------------------------------------===//
// Taking the ADDRESS of an explicit object member function.
//===----------------------------------------------------------------------===//

// `&S::get` on an explicit object member function yields a plain function pointer
// (`void (*)(S)`), not a pointer-to-member. The result therefore has an origin while
// the operand -- a reference to the function itself -- has none, and flowing one into
// the other asserted ("Dst is non-null but Src is null"). A source with no origins
// holds no borrow, so there is nothing to propagate and the destination correctly
// stays empty. An ordinary member function is a pointer-to-member and gives neither
// side origins; a static or free function gives both.
namespace address_of_explicit_object {
struct S {
  void get(this S) {}
  // expected-warning@+1 {{parameter that can hold a borrow is not annotated for lifetime safety}}
  void byref(this S &) {}
  void mem() {}      // ordinary member: pointer-to-member
  static void st() {}
};

// These carry no borrow, so there is nothing to diagnose -- the point is that none
// of them trips the assertion.
auto a1 = &S::get;    // no-warning
auto a2 = &S::byref;  // no-warning
auto a3 = &S::mem;    // no-warning
auto a4 = &S::st;     // no-warning
void (*a5)(S) = &S::get; // no-warning

// The object type holding a borrow does not change the answer: a function pointer
// carries none either way.
struct [[gsl::Pointer(char)]] V {
  const char *s;
  const char *ret(this V self [[clang::lifetimebound]]) { return self.s; }
};

auto a6 = &V::ret; // no-warning

void in_body() {
  // (A local initialized from one holds no borrow, so it draws the sentinel; that is
  // the ordinary treatment of an untracked value, not this fix.)
  auto p = &S::get;
  (void)p; // expected-warning {{lifetime safety cannot track local variable 'p' here}}
}
} // namespace address_of_explicit_object
