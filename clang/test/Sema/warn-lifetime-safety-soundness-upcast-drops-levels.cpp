// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// An upcast carries the object's loan, so that a base-member access reached through
// it still names the object. But the destination can be SHALLOWER than the source --
// converting a borrow-holding `Derived *` to a `Base *` whose pointee holds no
// origins -- and then the borrow the object carried has nowhere to go.
//
// Carrying the outer loan made that WORSE than silent: the destination is no longer
// empty, which masks the empty-origin sentinel that used to refuse the read. Mark the
// destination with an Unknown loan instead, so the loss is still visible.
//
// The mark has to JOIN rather than replace -- an IssueFact sets an origin's loan set
// outright, so marking after the flow would discard the loan just carried and a
// base-member access would project off Unknown, losing a report rather than adding
// one. And it is only for a CONVERSION: a [[clang::lifetimebound]] return is
// routinely shallower than its argument, and there nothing is lost (the outer loan
// plus the Interior step is the whole promise).

volatile char sink;

struct Task {
  virtual ~Task() = default;
  virtual void run() const = 0;
};

struct [[gsl::Pointer]] PrintTask : Task {
  const string *m;
  PrintTask(const string &s [[clang::lifetimebound]]) : m(&s) {}
  void run() const override { sink = m->data()[0]; }
};

//===----------------------------------------------------------------------===//
// Refused: the borrow cannot be represented through the base pointer.
//===----------------------------------------------------------------------===//

// The reported shape. `Task` holds no origins, so the borrow `PrintTask` carries is
// dropped by the conversion; the read through `t` must not be waved through.
void through_base_pointer() {
  Task *t = nullptr;
  {
    string s("temporary");
    t = new PrintTask(s);
  }
  t->run(); // expected-warning {{lifetime safety cannot track local variable 't' here}}
}

//===----------------------------------------------------------------------===//
// Tracked precisely, and must stay that way.
//===----------------------------------------------------------------------===//

// With the exact type there is no level to drop, so the borrow is followed and the
// dangle reported precisely.
void through_exact_pointer() {
  PrintTask *t = nullptr; // expected-warning {{uses more than one level of indirection}}
  {
    string s("temporary");
    t = new PrintTask(s); // expected-warning {{local variable 's' does not live long enough}}
  } // expected-note {{destroyed here}}
  t->run(); // expected-note {{later used here}}
}

// A base-member access reached through the implicit `this` upcast: the carried loan
// is what names the object, and marking must not displace it. (A view member bound to
// an owner member of a sibling base -- the self-referential report.)
struct [[gsl::Pointer(char)]] ViewBase {
  string_view v;
};
struct OwnerBase {
  string owned{"a string long enough to not be SSO"};
};

struct [[gsl::Pointer(char)]] Guard : ViewBase, OwnerBase {
  // expected-warning@+1 {{member is bound to a sibling member of the same object}}
  Guard() { v = owned; }
};

// A [[clang::lifetimebound]] accessor returning a shallower type than its object is
// the normal shape, not a loss: the result borrows into the object, which the outer
// loan plus the Interior step already says. Marking it would refuse every such
// accessor.
struct [[gsl::Pointer(char)]] Holder {
  char buf[8];
  const char *data() const [[clang::lifetimebound]] { return buf; }
};

void lifetimebound_return_is_not_a_loss() {
  Holder h{};
  // (`h` is default-initialized and so holds no borrow at all, which draws the
  // empty-origin sentinel on its own -- unrelated to the return's shape. The point
  // here is that the return does NOT additionally get marked as having lost one.)
  // expected-warning@+1 {{lifetime safety cannot track local variable 'h' here}}
  const char *p = h.data();
  sink = p[0]; // no-warning: `h` is alive, and nothing was lost at the return
}
