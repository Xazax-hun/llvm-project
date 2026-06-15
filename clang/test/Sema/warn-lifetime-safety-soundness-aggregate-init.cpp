// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Aggregate initialization of a [[gsl::Pointer]] / [[gsl::Owner]] record from
// its underlying pointer member -- `View{p}`, designated `View{.p = p}`, and
// the C++20 parenthesized `View(p)` -- was unmodeled: the aggregate's origin
// stayed empty and the captured borrow was dropped. When the destination object
// already held a loan (here the method's `this` placeholder, which never expires
// intra-procedurally), the lost-loan backstop was masked and a later dangling
// use of the member was missed. The aggregate init now merges each initializer's
// loans into the leaf object's own origin.

struct [[gsl::Pointer]] View {
  const char *p;
  unsigned n;
};

struct [[gsl::Pointer]] Holder {
  View v; // expected-note 4 {{this field dangles}}

  // Braced aggregate init storing a borrow of a local into the member view,
  // then using the view after the local dies.
  char braced() {
    {
      char local[64];
      v = View{local, 64}; // expected-warning {{escapes to the field 'v' which will dangle}}
    }
    return v.p[0];
  }

  // Designated initializer form.
  char designated() {
    {
      char local[64];
      v = View{.p = local, .n = 64}; // expected-warning {{escapes to the field 'v' which will dangle}}
    }
    return v.p[0];
  }

  // C++20 parenthesized aggregate init form.
  char paren() {
    {
      char local[64];
      v = View(local, 64); // expected-warning {{escapes to the field 'v' which will dangle}}
    }
    return v.p[0];
  }

  // Heap source freed through the member: invalidation.
  char heap() {
    const char *h = new char('A'); // expected-warning {{is later invalidated}}
    v = View{h, 1};
    delete h;      // expected-note {{freed here}}
    return v.p[0];
  }
};

// Negative: aggregating a long-lived borrow (a static buffer) stays silent.
struct [[gsl::Pointer]] HolderOK {
  View v;
  char ok() {
    static char buf[64];
    v = View{buf, 64};
    return v.p[0]; // no-warning
  }
};
