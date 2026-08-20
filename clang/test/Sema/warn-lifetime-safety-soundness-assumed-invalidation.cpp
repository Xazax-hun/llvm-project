// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// The analysis conservatively assumes that operations it cannot prove leave an
// owner unchanged invalidate borrows into that owner: non-const member calls,
// and passing an owner to a non-const pointer/reference parameter. The warning
// only fires when a borrow into the owner is actually live across the operation.

//===----------------------------------------------------------------------===//
// Case 1: non-const member call on an owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] MyBuf {
  int *data() [[clang::lifetimebound]]; // User accessor: NOT recognized as
                                        // non-invalidating (only std accessors
                                        // are), so assumed to invalidate.
  void touch();                         // Non-const, non-accessor.
  void look() const;
};

void member_call_warns() {
  MyBuf b;
  int *p = b.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b.touch();         // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
}

void const_member_silent() {
  MyBuf b;
  int *p = b.data();
  b.look(); // no-warning
  (void)p;
}

// A non-const method on a user owner is not on the std non-invalidating
// allow-list, so it is conservatively assumed to invalidate -- even a
// lifetimebound accessor (we cannot tell a user mutator from a user accessor).
void user_accessor_warns() {
  MyBuf b;
  int *p = b.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int *q = b.data(); // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
  (void)q;
}

// A recognized std accessor (operator[]) does NOT invalidate: a borrow held
// across another such access stays valid.
void std_accessor_silent() {
  vector<int> v;
  int &a = v[0];
  int &b = v[0]; // no-warning (std operator[] is on the allow-list)
  (void)a;
  (void)b;
}

//===----------------------------------------------------------------------===//
// Case 2: passing an owner to a non-const pointer/reference parameter.
//===----------------------------------------------------------------------===//

void mutate(vector<int> &v);
void inspect(const vector<int> &v);

void nonconst_owner_ref_warns() {
  vector<int> v;
  int &r = v[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate(v);     // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void const_owner_ref_silent() {
  vector<int> v;
  int &r = v[0];
  inspect(v); // no-warning
  (void)r;
}

void no_outstanding_borrow_silent() {
  vector<int> v;
  mutate(v); // no-warning
}

// A borrow invalidated by a known mutator is reported precisely (under
// -Wlifetime-safety-invalidation); the lower-confidence assumed-invalidation
// warning is suppressed for it even if a later non-const operation occurs.
void known_invalidation_suppresses_assumed() {
  vector<int> v;
  int &r = v[0];
  v.push_back(1);
  mutate(v); // no-warning
  (void)r;
}

//===----------------------------------------------------------------------===//
// Case 3: passing a gsl::Pointer (e.g. a span) that exposes mutable access to a
// non-const owner pointee can invalidate borrows into that owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] Str {
  int *data() [[clang::lifetimebound]];
};
template <class T> struct [[gsl::Pointer]] MutSpan {
  MutSpan(T *);
  T &operator[](int) const; // mutable owner element access
};
template <class T> struct [[gsl::Pointer]] ConstSpan {
  ConstSpan(const T *);
  const T &operator[](int) const; // const element access
};

void mutate_span(MutSpan<Str> s);
void read_span(ConstSpan<Str> s);

void mutable_owner_span_warns(Str *base) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Str *p = &base[0];
  MutSpan<Str> s(base);
  mutate_span(s); // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
}

void const_owner_span_silent(Str *base) {
  Str *p = &base[0];
  ConstSpan<Str> s(base);
  read_span(s); // no-warning
  (void)p;
}


//===----------------------------------------------------------------------===//
// Case 3: calling a lambda that captures an owner by reference. A by-reference
// capture gives the closure non-const access to the owner, so calling it is
// assumed to mutate the owner (like passing it to a non-const ref parameter).
//===----------------------------------------------------------------------===//

void lambda_ref_capture_warns() {
  vector<int> v;
  int &r = v[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  auto grow = [&v]() { v.push_back(1); };
  grow(); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void lambda_by_value_silent() {
  vector<int> v;
  int &r = v[0];
  auto grow = [v]() mutable { v.push_back(1); }; // by value: a copy
  grow();                                        // no-warning
  (void)r;
}

void lambda_no_capture_silent() {
  vector<int> v;
  int &r = v[0];
  auto noop = []() {};
  noop(); // no-warning
  (void)r;
}

//===----------------------------------------------------------------------===//
// A record that reaches a mutable owner through a non-const pointer/reference
// member (not only a by-value owner field) is treated as containing a mutable
// owner: a non-const method that mutates the owner through the indirection is
// assumed to invalidate borrows into that owner. Because the wrapper is a
// gsl::Pointer, the borrows it aliases live on its pointee origin, so the
// invalidation also reaches a borrow taken *directly* from the underlying owner
// (which carries the owner's loan, not the wrapper object's).
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] PtrWrap {
  vector<int> *v;
  PtrWrap(vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i);
  }
};

void ptr_member_owner_invalidated() {
  vector<int> d;
  d.push_back(42);
  PtrWrap w(&d);
  int &r = d[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  w.grow();      // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// A const pointee cannot be reallocated through the member, so no invalidation.
struct ConstPtrWrap {
  const vector<int> *v;
  ConstPtrWrap(const vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  const int *read() const { return v->data(); }
  void touch() {} // non-const, but cannot mutate *v
};

void const_ptr_member_silent() {
  vector<int> d;
  d.push_back(42);
  ConstPtrWrap w(&d);
  const int *x = w.read();
  w.touch(); // no-warning
  (void)x;
}

//===----------------------------------------------------------------------===//
// A gsl::Pointer wrapper that reaches a mutable owner is also owner-mutating
// when passed BY VALUE to a free function (not only as a method receiver): the
// copy still aliases the same owner, so a call that reallocates the owner
// through it invalidates a borrow taken directly from that owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] PtrWrap2 {
  vector<int> *v;
  PtrWrap2(vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
};

void grow_byval(PtrWrap2 w [[clang::noescape]]) {
  for (int i = 0; i < 1000; ++i)
    w.v->push_back(i);
}

void byval_wrapper_arg_invalidates() {
  vector<int> d;
  d.push_back(42);
  PtrWrap2 w(&d);
  int &r = d[0];   // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  grow_byval(w);   // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

//===----------------------------------------------------------------------===//
// A NESTED by-value gsl::Pointer wrapper: the borrowed owner is reached several
// pointee levels below the receiver (`w.in.grow()`: NestWrap -> PtrWrap2 ->
// vector). A gsl::Pointer record is a leaf in the origin tree, so a borrow
// captured into the whole object `w` lives on `w`'s own origin, not reachable
// from the freshly built `w.in` receiver origin. The receiver's origin records
// `w` as its parent, so invalidation walks the origin-tree parent chain and
// invalidates the enclosing object too.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] PtrWrapGrow {
  vector<int> *v;
  PtrWrapGrow(vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i);
  }
};

struct [[gsl::Pointer]] NestWrap {
  PtrWrapGrow in;
  NestWrap(PtrWrapGrow i [[clang::lifetime_capture_by(this)]]) : in(i) {}
};

void nested_wrapper_member_call_invalidates() {
  vector<int> d;
  d.push_back(42);
  NestWrap w{PtrWrapGrow{&d}};
  int &r = d[0];  // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  w.in.grow();    // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// Robustness: the enclosing object is found through the origin tree, not by
// AST-matching the base expression, so a non-trivial base form (here a
// conditional) is handled the same way.
void nested_wrapper_conditional_base(bool c) {
  vector<int> d;
  d.push_back(42);
  NestWrap a{PtrWrapGrow{&d}};
  NestWrap b{PtrWrapGrow{&d}};
  int &r = d[0];       // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  (c ? a : b).in.grow(); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

//===----------------------------------------------------------------------===//
// A pointer or reference to an ARRAY of owners reaches the elements, so it is
// owner-mutating in exactly the way a reference to a single owner is: the callee
// can reallocate `a[0]`. The array dimensions have to be peeled to see that --
// an array type is not itself a gsl::Owner and has no CXXRecordDecl, so testing
// the pointee directly finds no owner and the gate never opens.
//===----------------------------------------------------------------------===//

void mutate_arr(vector<int> (&a)[2]);
void mutate_arr_ptr(vector<int> (*a)[2]);
void read_arr(const vector<int> (&a)[2]);

void owner_array_ref_warns() {
  vector<int> a[2];
  a[0].push_back(42);
  int &r = a[0][0];  // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate_arr(a);     // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void owner_array_ptr_warns() {
  vector<int> a[2];
  a[0].push_back(42);
  int &r = a[0][0];    // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate_arr_ptr(&a);  // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// An array whose element is a record that *contains* an owner is the same
// situation: the peeled element type is what the record walk needs to see.
struct Box {
  vector<int> v;
};
void mutate_boxes(Box (&b)[2]);

void box_array_ref_warns() {
  Box b[2];
  b[0].v.push_back(42);
  int &r = b[0].v[0];  // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate_boxes(b);     // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// An array of CONST owners cannot be reallocated through the parameter. The
// qualifier sits on the element type, so it is only visible after peeling.
void const_owner_array_silent() {
  vector<int> a[2];
  a[0].push_back(42);
  int &r = a[0][0];
  read_arr(a); // no-warning
  (void)r;
}

// An array of non-owners reaches no owner at all.
void mutate_ints(int (&a)[2]);

void nonowner_array_silent() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  int ints[2];
  mutate_ints(ints); // no-warning
  (void)r;
}

// A guard's destructor can mutate the owner it captured, and the analysis cannot see that
// out-of-line body -- so destroying the guard is an assumed invalidation of the borrows it
// carries on that owner. That was modelled only where a NAMED local's scope ends. A
// temporary is destroyed by its full-expression cleanup instead, so forgetting to name the
// guard silenced the check, though the two forms differ only in the name.
struct [[gsl::Pointer]] Grower {
  vector<int> *vec;
  int tag = 0;
  ~Grower();
};

void unnamed_temporary_guard() {
  vector<int> v;
  v.push_back(42);
  // The report anchors at the borrow's CREATION, with the invalidating operation as a note.
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  (void)Grower{&v}.tag; // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// The byte-identical named local, which was always reported.
void named_guard() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  { Grower g{&v}; } // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// A borrow used only DURING the guard's lifetime is not invalidated by it: the destructor
// has not run yet.
void used_before_the_guard_dies() {
  vector<int> v;
  v.push_back(42);
  {
    Grower g{&v};
    int &r = v[0];
    (void)r; // no-warning
  }
}

// A guard that aliases only a const owner cannot mutate it.
struct [[gsl::Pointer]] ConstGuard {
  const vector<int> *vec;
  int tag = 0;
  ~ConstGuard();
};

void const_guard_temporary_silent() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  (void)ConstGuard{&v}.tag; // no-warning
  (void)r;
}

// A temporary whose value is DISCARDED is never materialized, so it has no
// MaterializeTemporaryExpr and its full-expression produces no cleanup element
// -- only a CFGTemporaryDtor. Skipping that element left an unnamed guard
// silent, though naming it or reading a member of it reported.
void discarded_temporary_guard() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  Grower{&v}; // expected-note {{assumed to be invalidated by this operation}}
  // expected-warning@-1 {{expression result unused}}
  (void)r;
}

void discarded_temporary_guard_cast_to_void() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  (void)Grower{&v}; // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// The same discarded form on a guard that cannot mutate stays silent.
void discarded_const_guard_silent() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  (void)ConstGuard{&v}; // no-warning
  (void)r;
}

// An ARRAY of guards destroys every element, so the element type decides the
// hazard exactly as for a single guard. The array type has no CXXRecordDecl, so
// testing it directly bailed out of the whole check and an array of guards was
// silent while the byte-identical scalar reported. The origin tree already shares
// one origin across an array's elements, so peeling the dimensions is all that is
// needed.
void array_of_guards() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  { Grower arr[1] = {Grower{&v}}; (void)arr; } // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// An array of guards that cannot mutate stays silent.
void array_of_const_guards() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  { ConstGuard arr[1] = {ConstGuard{&v}}; (void)arr; } // no-warning
  (void)r;
}

// Destroying a LAMBDA closure runs every capture's destructor, so an init-capture
// of a guard reallocates the borrowed owner when the closure dies. A closure is
// neither a pointer/reference nor a gsl::Pointer, so the mutation test said no;
// it is also exempt from the unknown-ownership ban (a lambda value is modeled
// directly) and carries no annotation, so nothing else covered it -- while the
// same guard held by an annotated wrapper, or by a plain struct, was reported.
void guard_captured_by_lambda() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  { auto f = [g = Grower{&v}] {}; (void)f; } // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// A by-value capture of an OWNER is a copy, so destroying it invalidates no borrow
// of the original.
void owner_captured_by_value() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  { auto f = [w = v] {}; (void)f; } // no-warning
  (void)r;
}

// A capture that cannot mutate stays silent too.
void const_guard_captured_by_lambda() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  { auto f = [g = ConstGuard{&v}] {}; (void)f; } // no-warning
  (void)r;
}
