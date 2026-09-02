// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;

// A callback passed BY VALUE can still reallocate the caller's owner, because a
// by-value copy of a closure copies its captures -- and a by-reference capture
// is an alias to the caller's object, not a copy of it. That made the mutation
// invisible: the parameter's type is a plain class, not a pointer or reference,
// so nothing in the signature said the call could mutate anything.
//
//   ws[0].withNotify([&ws](int i) { ws.push_back(...); });
//
// The receiver `ws[0]` borrows into the vector's buffer, the callback
// reallocates it, and the method then reads `this`. The identical hazard through
// an explicit owner argument -- `ws[0].go(ws)` -- was reported all along, so
// hiding the owner inside a lambda capture was the whole difference. The
// '[[clang::noescape]]' on the callback is TRUE and beside the point: the
// callback does not outlive the call, it reallocates during it.
//
// A record that OWNS its owner by value stays excluded: copying it copies the
// owner, so mutating the copy cannot reach the caller's.

volatile int sink;

struct Widget {
  int id = 7;
  template <class Fn> void withNotify(Fn fn [[clang::noescape]]) {
    fn(id);
    sink = id;
  }
};

// The reported shape.
void callback_reallocates_through_capture() {
  vector<Widget> ws;
  ws.emplace_back();
  ws[0].withNotify( // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
      // expected-note@-1 {{assumed to be invalidated by this operation}}
      [&ws](int i) { ws.push_back(Widget{}); (void)i; });
}

// A hand-written struct holding a reference to the owner is the same alias
// spelled out, and the predicate treats it the same way -- though in practice
// such a type is refused earlier, by unknown-ownership, for being annotated
// neither [[gsl::Owner]] nor [[gsl::Pointer]]. The closure above is the shape
// that reaches this check, because a closure type has no annotation to demand.

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// Capturing BY VALUE copies the container, so mutating the copy cannot reach
// the caller's buffer. This is the case that keeps the by-value exclusion.
void capture_by_value_is_a_copy() {
  vector<Widget> ws;
  ws.emplace_back();
  ws[0].withNotify([ws](int) mutable { ws.push_back(Widget{}); }); // no-warning
}

// No borrow of the container is live across the call, so there is nothing to
// invalidate -- the callback aliasing it is not by itself a hazard.
void no_live_borrow() {
  vector<Widget> ws;
  ws.emplace_back();
  Widget w;
  w.withNotify([&ws](int) { ws.push_back(Widget{}); }); // no-warning
}

// A callback that aliases an UNRELATED owner cannot invalidate this borrow.
void unrelated_owner() {
  vector<Widget> ws;
  vector<Widget> other;
  ws.emplace_back();
  ws[0].withNotify([&other](int) { other.push_back(Widget{}); }); // no-warning
}

// A callback holding no owner at all.
void callback_holds_nothing() {
  vector<Widget> ws;
  ws.emplace_back();
  int counter = 0;
  ws[0].withNotify([&counter](int i) { counter += i; }); // no-warning
}
