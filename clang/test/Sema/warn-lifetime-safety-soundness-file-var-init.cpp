// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
//
// Also covered in TU mode, but that mode additionally sweeps the test header's
// implicit string_view::operator= (a pre-existing report unrelated to this
// file), so it is not -verify'd here.
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -fexperimental-lifetime-safety-tu-analysis %s 2>&1 | FileCheck %s
// CHECK: warn-lifetime-safety-soundness-file-var-init.cpp:{{.*}}borrows from a mutable global

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A namespace-scope variable's dynamic initializer is code, but no entry point
// used to reach it: the per-declaration path runs when a FUNCTION scope is
// popped, and TU mode walks callables and the call graph -- a file-scope
// VarDecl is neither, and CallGraph does not descend into initializer
// statements. A borrow stored by such an initializer was therefore never
// analyzed: not refused, simply invisible.
//
// The bug this hides is a cross-global lifetime error. Initialization order
// within a TU is declaration order, so destruction order is the reverse: a
// variable declared FIRST is destroyed LAST, and can read a borrow of one
// declared later after that one is already gone.

volatile char sink;

string g_mut = "a mutable global string";

//===----------------------------------------------------------------------===//
// A store performed by a namespace-scope initializer is analyzed.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Watch {
  string_view v;
};
// The escape is reported against the borrow HOLDER's declaration, which is where
// the dangling value comes to rest.
Watch g_watch; // expected-warning {{borrows from a mutable global or static object}}

// The comma operator makes the initializer a statement sequence whose real work
// is the store. Nothing in it is a callable, so nothing used to analyze it.
// expected-warning@+1 {{cannot track global variable 'g_watch' here}}
static int g_wire = (g_watch.v = g_mut, 0);

// A borrow taken directly in the initializer, likewise.
Watch g_direct; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_direct' here}}
static int g_wire2 = (g_direct.v = g_mut, 0);

//===----------------------------------------------------------------------===//
// Nested contexts are reached too: namespaces, extern "C", and the out-of-class
// initializer of a static data member.
//===----------------------------------------------------------------------===//

namespace app {
Watch g_nested; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_nested' here}}
static int wire = (g_nested.v = g_mut, 0);
} // namespace app

namespace outer { namespace inner {
Watch g_deep; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_deep' here}}
static int wire = (g_deep.v = g_mut, 0);
}} // namespace outer::inner

extern "C" {
Watch g_extern_c; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_extern_c' here}}
static int g_wire3 = (g_extern_c.v = g_mut, 0);
}

struct Holder { static Watch s_watch; };
Watch Holder::s_watch; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track static variable 's_watch' here}}
static int g_wire4 = (Holder::s_watch.v = g_mut, 0);

//===----------------------------------------------------------------------===//
// A static data member of a class TEMPLATE is reached, for each instantiation.
//===----------------------------------------------------------------------===//

// Enumerating these variables by walking DeclContext::decls() by hand cannot
// reach an implicitly-instantiated specialization at all: it lives in the
// template's folding set, so it appears in NO enclosing decls() chain. And the
// dependent PATTERN is skipped -- correctly, since `static T t;` says nothing
// until T is known. Together those meant the initializer was analyzed for no
// instantiation whatsoever, which is why this needs a RecursiveASTVisitor with
// ShouldVisitTemplateInstantiations rather than a hand-rolled descent.

template <class T> struct PerType {
  static inline Watch s_watch; // expected-warning {{borrows from a mutable global or static object}}
  // expected-warning@+1 {{cannot track static variable 's_watch' here}}
  static inline int s_wire = (s_watch.v = g_mut, 0);
};
static int g_use = PerType<int>::s_wire;

// The out-of-line definition of such a member was already reached (it is a
// namespace-scope declaration in its own right); keep it here so the two forms
// stay together and neither regresses into reporting twice.
template <class T> struct OutOfLine {
  static Watch s_watch; // expected-warning {{borrows from a mutable global or static object}}
  static int s_wire;
};
template <class T> Watch OutOfLine<T>::s_watch;
// expected-warning@+1 {{cannot track static variable 's_watch' here}}
template <class T> int OutOfLine<T>::s_wire = (OutOfLine<T>::s_watch.v = g_mut, 0);
static int g_use2 = OutOfLine<char>::s_wire;

// A nested class inside a class template, and a CRTP base, are the same shape:
// the specialization that actually holds the initializer is implicit.
template <class T> struct Outer {
  struct Inner {
    static inline Watch s_watch; // expected-warning {{borrows from a mutable global or static object}}
    // expected-warning@+1 {{cannot track static variable 's_watch' here}}
    static inline int s_wire = (s_watch.v = g_mut, 0);
  };
};
static int g_use3 = Outer<int>::Inner::s_wire;

template <class D> struct CrtpBase {
  static inline Watch s_watch; // expected-warning {{borrows from a mutable global or static object}}
  // expected-warning@+1 {{cannot track static variable 's_watch' here}}
  static inline int s_wire = (s_watch.v = g_mut, 0);
};
struct Impl : CrtpBase<Impl> {};
static int g_use4 = Impl::s_wire;

//===----------------------------------------------------------------------===//
// A DEFAULT ARGUMENT is code in the same position.
//===----------------------------------------------------------------------===//

// It is owned by a declaration, runs whenever a call omits the argument, and is reached
// by no other entry point. The CFG deliberately does not descend into a
// CXXDefaultArgExpr -- the expression belongs to the callee's declaration, so adding it
// to each caller's CFG would make one Expr appear in several -- and the per-function path
// analyzes bodies, which a default argument is not. So a hazard written wholly inside one
// was invisible, while the identical expression at the call site is reported.
//
// Analyzing it once, at its declaration, is well defined: a default argument cannot name
// a local or another parameter, so only globals, static members and its own temporaries
// are in scope, and those mean the same thing from here as from any call site.

Watch g_dflt_free; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_dflt_free' here}}
void takes_free(int x = (g_dflt_free.v = g_mut, 0));

// A constructor's default argument, and a method's.
Watch g_dflt_ctor; // expected-warning {{borrows from a mutable global or static object}}
struct HasDefaultedCtor {
  // expected-warning@+1 {{cannot track global variable 'g_dflt_ctor' here}}
  HasDefaultedCtor(int x = (g_dflt_ctor.v = g_mut, 0));
};

Watch g_dflt_method; // expected-warning {{borrows from a mutable global or static object}}
struct HasDefaultedMethod {
  // expected-warning@+1 {{cannot track global variable 'g_dflt_method' here}}
  void go(int x = (g_dflt_method.v = g_mut, 0));
};

// A function declared inside a STATEMENT is reached too. The sweep skipped statements as a
// cost optimization -- justified for its other job, since a statement cannot contain a
// file-scope variable -- and default-argument analysis, layered on afterwards, silently
// inherited that. Every function nested in a statement was unreachable.

Watch g_dflt_lambda; // expected-warning {{borrows from a mutable global or static object}}
void in_a_lambda() {
  // expected-warning@+1 {{cannot track global variable 'g_dflt_lambda' here}}
  auto note = [](int x = (g_dflt_lambda.v = g_mut, 0)) { return x; };
  sink = (char)note();
}

// A lambda's call operator is not reached as a declaration at all: the traversal visits its
// body and parameters through the expression, never TraverseDecl on the operator. So this
// needed its own hook even after statements were walked, while the local class below did
// not.
Watch g_dflt_generic; // expected-warning {{borrows from a mutable global or static object}}
void in_a_generic_lambda() {
  // expected-warning@+1 {{cannot track global variable 'g_dflt_generic' here}}
  auto note = [](auto t, int x = (g_dflt_generic.v = g_mut, 0)) { return (int)t + x; };
  sink = (char)note(1);
}

Watch g_dflt_local; // expected-warning {{borrows from a mutable global or static object}}
Watch g_dflt_local_ctor; // expected-warning {{borrows from a mutable global or static object}}
void in_a_local_class() {
  struct Local {
    // expected-warning@+1 {{cannot track global variable 'g_dflt_local_ctor' here}}
    Local(int x = (g_dflt_local_ctor.v = g_mut, 0)) : n(x) {}
    // expected-warning@+1 {{cannot track global variable 'g_dflt_local' here}}
    int go(int x = (g_dflt_local.v = g_mut, 0)) { return n + x; }
    int n;
  };
  Local l;
  sink = (char)l.go();
}

// The function is at file scope; only the declaration carrying the default argument is in a
// DeclStmt, which is enough to hide it.
int block_scope_redecl(int);
Watch g_dflt_redecl; // expected-warning {{borrows from a mutable global or static object}}
void in_a_block_scope_redeclaration() {
  // expected-warning@+1 {{cannot track global variable 'g_dflt_redecl' here}}
  int block_scope_redecl(int x = (g_dflt_redecl.v = g_mut, 0));
  sink = (char)block_scope_redecl();
}
int block_scope_redecl(int x) { return x; }

// Negatives: a default argument that stores nothing and holds no borrow needs no CFG at
// all, which is what keeps this sweep from costing a CFG per default argument in the
// translation unit.
void cheap1(int x = 0);                     // no-warning
void cheap2(const char *p = nullptr);       // no-warning
void cheap3(int x = 40 + 2);                // no-warning
static int doubled(int a) { return a * 2; }
void cheap4(int x = doubled(3));            // no-warning
// A borrow of a literal is genuinely immortal.
void cheap5(const char *p = "literal");     // no-warning

//===----------------------------------------------------------------------===//
// Negatives: ordinary global initialization stays clean.
//===----------------------------------------------------------------------===//

// Owners initialized by value borrow nothing.
string g_name = "an ordinary global string, no borrow involved here";
int g_count = 42;

// A string literal has static storage duration and is genuinely immortal.
const char *g_literal = "literals never dangle";

// A constant initializer performs no runtime store at all.
constexpr int g_const = 7;

// An aggregate of owners.
struct Cfg { string name; int n; };
Cfg g_cfg{"config", 3};
