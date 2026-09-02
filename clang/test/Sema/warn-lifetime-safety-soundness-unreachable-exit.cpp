// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

// Liveness is a BACKWARD analysis, and it walked predecessors from the exit
// block -- so it only ever reached code that can REACH the exit. Code that
// cannot was left with no state at all, and a use written there was invisible.
//
// That is silence, not conservatism: liveness is what the expiry and
// invalidation checks intersect against, so a borrow used only where the exit
// is unreachable looked dead and its dangling use went unreported. An event
// loop (`for (;;)`) and a [[noreturn]] worker are the ordinary spellings, and
// in the second the WHOLE function is affected -- the exit has no predecessors
// at all, so nothing in the body was analyzed, including code before the loop.
//
// Every block the exit-rooted walk misses is now seeded and the fixpoint
// finished. Bottom is what the exit starts from and joins only add, so the rest
// of the function reaches the same answer as before.

volatile char sink;
volatile int isink;

[[noreturn]] void bail();

// The borrow dies before a loop that never exits, and is used inside it.
void use_in_infinite_loop() {
  string_view banner;
  {
    string cfg = "a long heap string value exceeding the sso buffer now!!";
    banner = cfg; // expected-warning {{local variable 'cfg' does not live long enough}}
  }               // expected-note {{destroyed here}}
  for (;;) {
    sink = banner.data()[0]; // expected-note {{later used here}}
  }
}

// `while (true)` is the same CFG shape.
void use_in_while_true() {
  string_view banner;
  {
    string cfg = "a long heap string value exceeding the sso buffer now!!";
    banner = cfg; // expected-warning {{local variable 'cfg' does not live long enough}}
  }               // expected-note {{destroyed here}}
  while (true) {
    sink = banner.data()[0]; // expected-note {{later used here}}
  }
}

// So is a backward goto.
void use_in_goto_loop() {
  string_view banner;
  {
    string cfg = "a long heap string value exceeding the sso buffer now!!";
    banner = cfg; // expected-warning {{local variable 'cfg' does not live long enough}}
  }               // expected-note {{destroyed here}}
again:
  sink = banner.data()[0]; // expected-note {{later used here}}
  goto again;
}

// The exit can also be unreachable because the function ends in a [[noreturn]]
// call rather than a loop.
void use_before_noreturn_call() {
  string_view banner;
  {
    string cfg = "a long heap string value exceeding the sso buffer now!!";
    banner = cfg; // expected-warning {{local variable 'cfg' does not live long enough}}
  }               // expected-note {{destroyed here}}
  sink = banner.data()[0]; // expected-note {{later used here}}
  bail();
}

// The hazard need not be inside the loop: in a function that never returns,
// nothing was analyzed, so a plain invalidation BEFORE the loop was missed too.
void invalidation_before_infinite_loop() {
  vector<int> buf;
  buf.push_back(1);
  int *p = &buf[0];   // expected-warning {{object whose reference is captured is later invalidated}}
  buf.push_back(2);   // expected-note {{invalidated here}}
  isink = *p;         // expected-note {{later used here}}
  for (;;) {
  }
}

// Reached only on a branch that the loop takes some of the time.
void use_in_infinite_loop_conditional(bool c) {
  string_view banner;
  {
    string cfg = "a long heap string value exceeding the sso buffer now!!";
    banner = cfg; // expected-warning {{local variable 'cfg' does not live long enough}}
  }               // expected-note {{destroyed here}}
  for (;;) {
    if (c)
      sink = banner.data()[0]; // expected-note {{later used here}}
  }
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The borrow outlives the loop's use.
void valid_borrow_in_infinite_loop() {
  string cfg = "a long heap string value exceeding the sso buffer now!!";
  string_view banner = cfg;
  for (;;) {
    sink = banner.data()[0]; // no-warning
  }
}

// An ordinary loop that does reach the exit was always handled.
void terminating_loop() {
  string cfg = "a long heap string value exceeding the sso buffer now!!";
  string_view banner = cfg;
  for (int i = 0; i < 10; ++i) {
    sink = banner.data()[0]; // no-warning
  }
}

// Code unreachable from ENTRY is not analyzed at all; seeding the backward walk
// must not start reporting in it.
void dead_code() {
  return;
  {
    string_view banner;
    {
      string cfg = "a long heap string value exceeding the sso buffer now!!";
      banner = cfg; // no-warning
    }
    sink = banner.data()[0];
  }
}
