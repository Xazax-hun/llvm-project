// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -I %t -verify=on %t/main.cpp
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -I %t -verify=off %t/main.cpp

// Whether a function is analyzed at all used to be decided from the diagnostic
// state at its FIRST TOKEN. A pragma region ending inside the declaration -- or
// starting inside its body -- re-enables the warnings for the rest of it, and
// skipping the analysis then dropped every finding there with no diagnostic.

//--- enable.h
#pragma clang diagnostic warning "-Wlifetime-safety-use-after-scope"

//--- main.cpp
volatile int sink;

// Disabled at the first token only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety"
void
#pragma clang diagnostic pop
off_at_first_token() {
  int *p;
  { int x = 1; p = &x; } // on-warning {{local variable 'x' does not live long enough}} \
                         // on-note {{destroyed here}}
  sink = *p;             // on-note {{later used here}}
}

// Disabled until a pop inside the body.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety"
void off_until_body() {
#pragma clang diagnostic pop
  int *p;
  { int x = 1; p = &x; } // on-warning {{local variable 'x' does not live long enough}} \
                         // on-note {{destroyed here}}
  sink = *p;             // on-note {{later used here}}
}

// Off on the command line, enabled by a pragma inside the body.
void enabled_in_body() {
#pragma clang diagnostic push
#pragma clang diagnostic warning "-Wlifetime-safety-use-after-scope"
  int *p;
  { int x = 1; p = &x; } // on-warning {{local variable 'x' does not live long enough}} \
                         // on-note {{destroyed here}} \
                         // off-warning {{local variable 'x' does not live long enough}} \
                         // off-note {{destroyed here}}
  sink = *p;             // on-note {{later used here}} off-note {{later used here}}
#pragma clang diagnostic pop
}

// Off on the command line, enabled by a pragma in a file included from inside
// the body.
void enabled_by_include() {
#include "enable.h"
  int *p;
  { int x = 1; p = &x; } // on-warning {{local variable 'x' does not live long enough}} \
                         // on-note {{destroyed here}} \
                         // off-warning {{local variable 'x' does not live long enough}} \
                         // off-note {{destroyed here}}
  sink = *p;             // on-note {{later used here}} off-note {{later used here}}
}
