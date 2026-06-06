// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// std::string concatenation operators deep-copy their operands' characters, so
// an operand (a string reference or a character pointer) does not escape. Under
// the safe programming model the call must not be flagged as an unannotated
// indirection -- mirroring container insertion methods like push_back. (Locals
// are used so the test exercises the operator argument, not parameter
// annotation, which is a separate requirement.)

void append() {
  std::string a, b;
  a += b;         // no-warning
  a += "literal"; // no-warning
  a += 'x';       // no-warning
}

void concat() {
  std::string a, b;
  std::string c = a + b; // no-warning
  c = a + "literal";     // no-warning
  c = "literal" + b;     // no-warning
  c = a + 'x';           // no-warning
}

// push_back of a non-borrow element is likewise clean (pre-existing behavior).
void insertion() {
  std::vector<int> v;
  int x = 5;
  v.push_back(x); // no-warning
}

// Over-broadening guard: pushing a *borrow* into a container still escapes, so
// the safe model surfaces it (string concatenation copies, container insertion
// of a pointer captures).
void push_back_borrow_still_flagged() {
  std::vector<int *> v; // expected-warning {{is a container whose element type is a pointer or reference}}
  int x = 5;
  v.push_back(&x); // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated}}
}

