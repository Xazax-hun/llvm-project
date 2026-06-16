// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// The argument-overlap check now uses storage CONTAINMENT, not just equality: a
// co-argument that borrows a SUBOBJECT of the mutated owner aliases it too, since
// mutating the owner may reallocate any owner field it (transitively) contains.
// The key case: a method mutates its own owner field and takes a view parameter
// that, at the call site, aliases that field. The view is not live after the
// call, so the liveness-based invalidation pass misses it; the overlap check
// (about the call's internal aliasing) catches it.
struct S {
  string buf;
  void process(string_view v [[clang::noescape]]) {
    buf.push_back('z');  // reallocates this->buf
    sink = *v.data();    // v aliased this->buf -> dangling
  }
  void run() {
    process(buf); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}} expected-note {{assumed to be invalidated by this operation}}
  }
};

// Negative: a view of a SEPARATE owner (a local, not a subobject of the mutated
// receiver) is not reachable from it -> no overlap hazard.
struct T {
  string buf;
  void process(string_view v [[clang::noescape]]) {
    buf.push_back('z');
    sink = *v.data();
  }
  void run_ok() {
    string other;
    process(other); // no-warning: 'other' is not a subobject of the receiver
  }
};
