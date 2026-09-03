// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

// A [[gsl::Owner]]'s members used to be opaque, so a borrow parked in one was
// dropped: it landed on a transient member-access origin, and every downstream
// check -- expiry, invalidation, escape -- then had nothing to see. Only a store
// through `this` or a parameter was reported, by checks that judge the store
// itself; six other destinations were silent.
//
// A non-public member of an owner written in THIS TU now gets an origin, so the
// borrow has somewhere to land and the ordinary machinery does the reasoning. A
// library owner stays opaque -- std::string's private pointers are
// implementation details, and modelling them would be both wrong and unbounded.
//
// That also means an owner now HAS origins, which is why the indirection depth
// no longer counts a record as a level: otherwise `Owner &` would measure the
// same as `int **` and every out-parameter of an owner would be refused. A
// record that IS a borrow (a [[gsl::Pointer]] view) still counts, so `View &`
// stays refused.

volatile char sink;
volatile int isink;

class [[gsl::Owner(char)]] Box {
  const char *d = nullptr;

public:
  char peek() const { return d[0]; }
  // Unannotated on purpose: the point below is the out-parameter's depth, not
  // this parameter, and the analysis duly asks for an annotation here.
  void set(const char *s) { d = s; } // expected-warning {{parameter that can hold a borrow is not annotated}}
  friend void into_local();
  friend Box factory();
  friend void into_global();
  friend void invalidated();
  friend void read_while_alive();
};

// The borrow outlives what it points at, read after the source dies.
void into_local() {
  Box b;
  {
    string t = "a long heap string value exceeding the sso buffer now";
    b.d = t.c_str(); // expected-warning {{local variable 't' does not live long enough}}
  }                  // expected-note {{destroyed here}}
  sink = b.peek();   // expected-note {{later used here}}
}

// The factory idiom: an owner returned holding a borrow of a body local.
Box factory() {
  Box b;
  string l = "a long heap string value exceeding the sso buffer now";
  b.d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' is returned}}
  return b;        // expected-note {{returned here}}
}

// Into a global.
Box g_box; // expected-note {{this global dangles}}
void into_global() {
  string l = "a long heap string value exceeding the sso buffer now";
  g_box.d = l.c_str(); // expected-warning {{escapes to the global}}
}

// Invalidation reaches it too, now that the member carries the loan.
class [[gsl::Owner(int)]] IntBox {
  const int *hot = nullptr;

public:
  friend void invalidated();
};

void invalidated() {
  vector<int> data;
  data.push_back(1);
  IntBox c;
  c.hot = &data[0];  // expected-warning {{object whose reference is captured is later invalidated}}
  data.push_back(4); // expected-note {{invalidated here}}
  isink = *c.hot;    // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// An `Owner &` out-parameter is ONE level of indirection and must be usable --
// this is the case the depth change exists for.
// The out-parameters themselves draw no indirection refusal, which is the point.
void owner_out_param(Box &out) { // expected-warning {{parameter that can hold a borrow is not annotated}}
  out.set("an immortal string literal"); // expected-warning {{argument is bound to a parameter that can hold a borrow}}
}

void owner_out_pointer(Box *out) { // expected-warning {{parameter that can hold a borrow is not annotated}}
  out->set("an immortal string literal"); // expected-warning {{argument is bound to a parameter that can hold a borrow}}
}

// The read precedes the local's death, so nothing dangles. This is what the
// expiry reasoning buys over judging the store: a store-site rule would have to
// report this too.
void read_while_alive() {
  Box b;
  string l = "a long heap string value exceeding the sso buffer now";
  b.d = l.c_str();
  sink = b.peek(); // no-warning
}

// A library owner stays opaque: its private members are not modelled.
void library_owner_untouched() {
  string s = "a long heap string value exceeding the sso buffer now";
  sink = s.c_str()[0]; // no-warning
}
