// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

// No lifetime annotation expresses that two arguments must not alias. Passing an
// owner the call may mutate together with a view that borrows it (`f(s, v)` with
// `string_view v = s;`) is a hazard: the callee may reallocate the owner and
// then use the now-dangling view, in an order the caller cannot see. The call
// site is flagged when the two arguments' borrows actually alias.

void mutate(string &s [[clang::noescape]], string_view v [[clang::noescape]]);
void inspect(const string &s [[clang::noescape]], string_view v [[clang::noescape]]);
void byValue(string s [[clang::noescape]], string_view v [[clang::noescape]]);

void overlap_warns() {
  string s = "hello world";
  string_view v = s;       // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate(s, v);            // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Negatives: must NOT fire.
//===----------------------------------------------------------------------===//

// The view borrows a *different* owner.
void distinct_owners_ok() {
  string s = "a";
  string s2 = "b";
  string_view v = s2;
  mutate(s, v); // no-warning (v does not alias s)
}

// The owner is passed by const reference: the call cannot mutate it.
void const_ref_ok() {
  string s = "a";
  string_view v = s;
  inspect(s, v); // no-warning
}

// The owner is passed by value: the original is not mutated by the call.
void by_value_ok() {
  string s = "a";
  string_view v = s;
  byValue(s, v); // no-warning
}

// Only the owner is passed; the view is independent.
void no_view_arg_ok() {
  string s = "a";
  mutate(s, string_view{}); // no-warning
}

//===----------------------------------------------------------------------===//
// A mutating method receiver counts as the mutated argument too.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] Buf {
  int *data() [[clang::lifetimebound]];
  void grow(int *p [[clang::noescape]]); // non-const: may reallocate *this
};

void receiver_overlap_warns() {
  Buf b;
  int *p = b.data(); // p borrows b   expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b.grow(p);         // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// The mutated argument need not be an owner itself: a non-owner object passed
// by mutable reference that CONTAINS an owner field may reallocate it.
//===----------------------------------------------------------------------===//

struct Widget {
  string buf;
};
void mutateWidget(Widget &w [[clang::noescape]], string_view v [[clang::noescape]]);

void contains_owner_field_warns() {
  Widget w;
  string_view v = w.buf; // v borrows w.buf   expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutateWidget(w, v);    // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// A known container mutator as the receiver counts too (`s.append(view_of_s)`).
//===----------------------------------------------------------------------===//

void known_mutator_receiver_warns() {
  string s = "hello";
  string_view v = s; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  s.insert(0, v);    // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Assembling an AGGREGATE brings borrows together exactly as a call does.
//===----------------------------------------------------------------------===//

// A mutable handle to an owner and a view of that same owner may not exist at once, and
// building an aggregate out of them combines them at one point just as passing them to a
// function does. The question was asked only at calls, so the identical arguments were
// reported through a constructor and a free function but not through braces -- which is how
// a mutating member and a borrowing member could be assembled into one object silently, the
// mutating one then reallocating what the other reads when it is destroyed first.
struct [[gsl::Pointer]] Viewer {
  string_view text;
};
struct [[gsl::Pointer]] Mutator {
  string *buf;
};

void aggregate_init_warns() {
  string s = "hello";
  struct Both {
    Viewer v;
    Mutator m;
  };
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Both b{Viewer{s}, Mutator{&s}}; // expected-note {{assumed to be invalidated by this operation}}
  (void)b;
}

// The controls, which were always reported: the same two arguments to a constructor and to
// a free function.
struct TakesBoth {
  TakesBoth(Viewer v, Mutator m);
};
void takes_both(Viewer v, Mutator m);

void constructor_warns() {
  string s = "hello";
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  TakesBoth t{Viewer{s}, Mutator{&s}}; // expected-note {{assumed to be invalidated by this operation}}
  (void)t;
}

void free_function_warns() {
  string s = "hello";
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  takes_both(Viewer{s}, Mutator{&s}); // expected-note {{assumed to be invalidated by this operation}}
}

// Negatives: two views of the same owner alias nothing mutable, and a mutable handle to a
// DIFFERENT owner cannot invalidate this view.
void two_views_silent() {
  string s = "hello";
  struct TwoViews {
    Viewer a;
    Viewer b;
  };
  TwoViews t{Viewer{s}, Viewer{s}}; // no-warning
  (void)t;
}

void unrelated_owner_silent() {
  string s = "hello";
  string other = "world";
  struct Both {
    Viewer v;
    Mutator m;
  };
  Both b{Viewer{s}, Mutator{&other}}; // no-warning
  (void)b;
}

// A union initializes one member, so there are no siblings to overlap with.
void union_silent() {
  string s = "hello";
  union U {
    Viewer v;
    Mutator m;
  };
  U u{Viewer{s}}; // no-warning
  (void)u;
}
