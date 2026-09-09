// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;

// An origin mentioned in more than one CFG block has to survive the join between
// them; one mentioned in a single block is kept block-local, and its loans are
// dropped at the boundary. computePersistentOrigins decides which by walking the
// facts and registering the origins each one names -- so a fact that names an origin
// without registering it makes that origin look block-local, and the fact then sees
// an empty origin and concludes there is nothing to say.
//
// InvalidateOrigin did not register the origin it invalidates. It usually got away
// with it because the receiver is re-issued in the block that mutates it, but a
// STRUCTURED BINDING expands every use to the same MemberExpr, so one origin carries
// the member for the whole function. Put the mutation in a loop and the invalidation
// names an origin projected in an earlier block: no loans, no report -- while the
// identical loop written `rec.samples` re-projects in the loop body and was reported.
//
// Projection, FieldStore and ArgumentOverlap were missing from the same switch.

volatile int sink;

struct Record {
  vector<int> samples;
};

//===----------------------------------------------------------------------===//
// The mutation is in a different block from where the origin was established.
//===----------------------------------------------------------------------===//

// The reported shape: binding + mutation inside a loop.
void binding_mutated_in_loop() {
  Record rec;
  // The borrow of `rec` is created by the binding declaration, so that is where
  // the report anchors.
  auto &[samples] = rec; // expected-warning {{object whose reference is captured is later invalidated}}
  int *first = &samples[0];
  for (int i = 0; i < 2; ++i)
    samples.push_back(i); // expected-note {{invalidated here}}
  sink = *first;          // expected-note {{later used here}}
}

// Mutation under an `if`, which is also a separate block.
void binding_mutated_in_branch(bool c) {
  Record rec;
  // The borrow of `rec` is created by the binding declaration, so that is where
  // the report anchors.
  auto &[samples] = rec; // expected-warning {{object whose reference is captured is later invalidated}}
  int *first = &samples[0];
  if (c)
    samples.push_back(1); // expected-note {{invalidated here}}
  sink = *first;          // expected-note {{later used here}}
}

// Without the binding the receiver is re-issued in the loop body, so this was
// reported all along -- the two must agree.
void member_mutated_in_loop() {
  Record rec;
  int *first = &rec.samples[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  for (int i = 0; i < 2; ++i)
    rec.samples.push_back(i); // expected-note {{invalidated here}}
  sink = *first;              // expected-note {{later used here}}
}

// Single-block spelling, reported all along.
void binding_mutated_same_block() {
  Record rec;
  // The borrow of `rec` is created by the binding declaration, so that is where
  // the report anchors.
  auto &[samples] = rec; // expected-warning {{object whose reference is captured is later invalidated}}
  int *first = &samples[0];
  samples.push_back(1);     // expected-note {{invalidated here}}
  sink = *first;            // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: the borrow is taken after the mutation.
//===----------------------------------------------------------------------===//

void borrow_after_loop() {
  Record rec;
  auto &[samples] = rec;
  for (int i = 0; i < 2; ++i)
    samples.push_back(i);
  int *first = &samples[0];
  sink = *first; // no-warning
}
