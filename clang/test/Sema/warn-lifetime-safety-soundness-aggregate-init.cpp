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
  // The reads below (`v.p[0]`) register a use of the member's borrow, so these
  // are reported as a use-after-scope / use-after-free at the read rather than
  // only as "escapes to a field that will dangle": the analysis can now name the
  // borrow, the destruction, and the later use.
  View v;

  // Braced aggregate init storing a borrow of a local into the member view,
  // then using the view after the local dies.
  char braced() {
    {
      char local[64];
      v = View{local, 64}; // expected-warning {{local variable 'local' does not live long enough}}
    } // expected-note {{destroyed here}}
    return v.p[0]; // expected-note {{later used here}}
  }

  // Designated initializer form.
  char designated() {
    {
      char local[64];
      v = View{.p = local, .n = 64}; // expected-warning {{local variable 'local' does not live long enough}}
    } // expected-note {{destroyed here}}
    return v.p[0]; // expected-note {{later used here}}
  }

  // C++20 parenthesized aggregate init form.
  char paren() {
    {
      char local[64];
      v = View(local, 64); // expected-warning {{local variable 'local' does not live long enough}}
    } // expected-note {{destroyed here}}
    return v.p[0]; // expected-note {{later used here}}
  }

  // Heap source freed through the member: now a use-after-free at the read.
  char heap() {
    const char *h = new char('A'); // expected-warning {{allocated object does not live long enough}}
    v = View{h, 1};
    delete h;      // expected-note {{freed here}}
    return v.p[0]; // expected-note {{later used here}}
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

//===----------------------------------------------------------------------===//
// Reference members and base subobjects.
//===----------------------------------------------------------------------===//

// A reference member binds to the initializer's storage (a borrow of it). The
// init expression is the referent glvalue, so it must use the unpeeled origin;
// otherwise the borrow is dropped. With a base subobject present, the field
// iterator must skip the leading base initializer to still recognize the
// reference field -- otherwise the borrow was silently dropped and, masked by a
// sibling pointer member's valid loan, the dangling reference was missed.
int g_ok = 99;
struct Base {};
struct [[gsl::Pointer]] RefView : Base {
  const int *p;
  const int &r;
};

const int &ref_with_base_dangles() {
  int local = 5;
  RefView v{Base{}, &g_ok, local}; // expected-warning {{stack memory associated with local variable 'local' is returned}}
  return v.r;                      // expected-note {{returned here}}
}

// Negative: the same shape with a long-lived (global) reference stays silent.
int g_other = 7;
const int &ref_with_base_ok() {
  RefView v{Base{}, &g_ok, g_other};
  return v.r; // no-warning
}

