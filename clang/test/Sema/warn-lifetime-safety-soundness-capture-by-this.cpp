// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A [[clang::lifetime_capture_by(this)]] setter stashes the argument into the
// object. When the capture, the captured local's destruction, and a later read
// of the object all happen inside one method (the object outlives the local),
// the captured borrow dangles. The capture is modeled as a flow into the
// whole-object `this` origin (we do not know which member holds it); keeping
// `this` live at function exit lets the expiry check catch the captured local
// going out of scope while still held.

struct Latch {
  string_view last;
  void set(string_view sv [[clang::lifetime_capture_by(this)]]) { last = sv; }

  void capture_then_dangle() {
    {
      string tmp = "a long heap string value exceeding the sso buffer now!!";
      set(tmp); // expected-warning {{does not live long enough}}
    } // expected-note {{destroyed here}}
    const char *p = last.data(); // expected-note {{later used here}}
    (void)p;
  }
};

// Negative: the captured local outlives the read, so no dangle.
void captured_outlives_use() {
  string keep = "a long heap string value exceeding the sso buffer now!!";
  Latch l;
  l.set(keep);
  const char *p = l.last.data(); // no-warning
  (void)p;
}
