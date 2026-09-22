// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety -verify %s

// A lifetime report's primary diagnostic is anchored at the borrow's CREATION --
// the one location every loan has, and what the message names ("parameter is
// later invalidated"). Clang consults the diagnostic state at the location a
// diagnostic is emitted AT, so a `#pragma clang diagnostic ignored` placed around
// the offending USE had no effect whatsoever.
//
// For a borrow of a PARAMETER that is not merely surprising, it is inescapable:
// the creation is the parameter declaration, so no pragma written anywhere in the
// body could ever suppress the report. Reported from the field on exactly that
// shape.
//
// A report is now also suppressed when it is silenced at the code it is ABOUT,
// not only at the code it points at.

struct S {
  int id;
  ~S();
};

void deallocate(S *);
volatile int sink;

//===----------------------------------------------------------------------===//
// The reported case: a template whose borrow is a parameter, with the pragma
// around the offending call.
//===----------------------------------------------------------------------===//

template <typename T> void destroy(T *p) {
  p->~T();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-invalidation"
  deallocate(p); // no-warning
#pragma clang diagnostic pop
}

void instantiate_destroy(S *p) { destroy(p); }

//===----------------------------------------------------------------------===//
// The same shape without a template, and the control that must still report.
//===----------------------------------------------------------------------===//

void suppressed_at_use(S *p) {
  p->~S();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-invalidation"
  sink = p->id; // no-warning
#pragma clang diagnostic pop
}

void not_suppressed(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                  // expected-note {{invalidated here}}
  sink = p->id;             // expected-note {{later used here}}
}

// Silencing a DIFFERENT group must not suppress it -- the guard keys on the same
// diagnostic ID, not on the presence of any pragma.
void other_group_ignored(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                       // expected-note {{invalidated here}}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
  sink = p->id; // expected-note {{later used here}}
#pragma clang diagnostic pop
}

//===----------------------------------------------------------------------===//
// The same for use-after-scope, whose anchor is the borrow rather than a
// parameter declaration -- so a pragma around the use was equally ineffective.
//===----------------------------------------------------------------------===//

void scope_suppressed_at_use() {
  int *p;
  {
    int x = 1;
    p = &x;
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-use-after-scope"
  sink = *p; // no-warning
#pragma clang diagnostic pop
}

void scope_not_suppressed() {
  int *p;
  {
    int x = 1;
    p = &x;  // expected-warning {{local variable 'x' does not live long enough}}
  }          // expected-note {{destroyed here}}
  sink = *p; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// NOT changed, and worth recording: a pragma around the CREATION still suppresses,
// because that is the location clang's own state lookup uses. It therefore also
// silences a use that sits outside the region -- the mirror image of the bug
// above. Making that behave would mean moving the primary diagnostic to the use,
// which relocates every lifetime warning in the suite; it is deliberately left
// alone here.
//===----------------------------------------------------------------------===//

void suppressed_at_creation() {
  S s;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-invalidation"
  S *p = &s;
#pragma clang diagnostic pop
  p->~S();
  sink = p->id; // no-warning: silenced by the pragma around the borrow
}
