// RUN: %clang_cc1 -fsyntax-only -std=c++23 -I%S/Inputs -Wlifetime-safety-soundness -verify %s

#include "lifetime-analysis.h"

volatile int sink;
volatile char csink;

// An object survives a mutation of its own contents, so a holder pointing AT a
// container is not dangled by `v->push_back()`; only a holder pointing INTO it is.
// That exemption is granted only on positively recognising a pointer AT the
// mutated record, which needs the record the mutated storage denotes --
// `invalidatedObjectRecord`.
//
// A MEMBER lost the exemption. Its loan has an `Uninitialized` root: the analysed
// method did not initialise it (the constructor ran elsewhere), so no borrow is
// *known* to be stored. That says nothing about the storage's TYPE, but the
// `Uninitialized` root was the one root whose declared type was not consulted, so
// no record came back at all and `originBorrowsInto` had to answer conservatively
// that the holder points INTO the container. The same code with the pointer in a
// local or a parameter -- roots that do carry their type -- was correctly silent.

//===----------------------------------------------------------------------===//
// Pointing AT the container: no diagnostic.
//===----------------------------------------------------------------------===//

struct PointerMember {
  std::vector<int> *v;
  void grow() { v->push_back(1); }
  // Reading through the AT-pointer after the mutation is still fine: the
  // reallocation moved the buffer, not the vector.
  void grow_then_read() {
    v->push_back(1);
    sink = (*v)[0];
  }
};

struct ReferenceMember {
  std::vector<int> &v;
  void grow_then_read() {
    v.push_back(1);
    sink = v[0];
  }
};

// The container held BY VALUE as a member: mutating it through `this` is the
// object mutating its own contents.
struct OwnerMember {
  std::vector<int> v;
  void grow_then_read() {
    v.push_back(1);
    sink = v[0];
  }
};

// The local and parameter spellings, which always worked -- kept so the three
// roots cannot drift apart again.
void at_pointer_in_local(std::vector<int> &vec [[clang::noescape]]) {
  std::vector<int> *p = &vec;
  p->push_back(1);
  sink = (*p)[0];
}

void at_pointer_in_param(std::vector<int> *p [[clang::noescape]]) {
  p->push_back(1);
  sink = (*p)[0];
}

//===----------------------------------------------------------------------===//
// Pointing INTO the container: must still be reported. The exemption keys on the
// holder's type, so giving the `Uninitialized` root a record does not spare these.
//===----------------------------------------------------------------------===//

struct RawPointerIntoBuffer {
  std::vector<int> *v;
  int *elem;
  void bug() {
    elem = &(*v)[0];
    v->push_back(1); // expected-note {{invalidated here}}
    // The borrow carries no issuing expression of its own, so the report anchors
    // at the use that keeps it live.
    sink = *elem; // expected-warning {{is later invalidated}} \
                  // expected-note {{later used here}}
  }
};

struct ViewIntoOwner {
  std::string *s;
  std::string_view sv;
  void bug() {
    sv = *s;
    s->push_back('x'); // expected-note {{invalidated here}}
    csink = *sv.data(); // expected-warning {{is later invalidated}} \
                        // expected-note {{later used here}}
  }
};

struct IteratorIntoContainer {
  std::vector<int> *v;
  std::vector<int>::iterator it;
  void bug() {
    it = v->begin();
    v->push_back(1); // expected-note {{invalidated here}}
    sink = *it;      // expected-warning {{is later invalidated}} \
                     // expected-note {{later used here}}
  }
};
