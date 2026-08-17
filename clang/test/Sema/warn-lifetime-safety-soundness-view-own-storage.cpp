// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// For a [[gsl::Pointer]] receiver, a member named `data` / `get` / `c_str` /
// `begin` / `end` / `cbegin` is treated as handing out a borrow of what the view
// points AT rather than of the view object. That is a heuristic about the standard
// library, where it holds -- `string_view::data()` really does return a pointer
// into the viewed buffer -- but it is only a guess about the NAME.
//
// A user-defined view can have storage of its own, and a member named `data` may
// return a pointer to the object's own bytes. The guess then credited the borrow
// to the viewed object, which typically outlives the view, so a use after the view
// died was reported nowhere -- and it overrode an explicit
// '[[clang::lifetimebound]]' saying the result refers to THIS object.
//
// So an explicit annotation wins over the guess. No standard library view
// annotates these accessors, so nothing changes for the STL (checked below).

//===----------------------------------------------------------------------===//
// A user view handing out a borrow of its own inline storage.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Normalized {
  string_view src;
  char scratch[64];

  // Truthful, and verifiable: the result really does point into `*this`.
  const char *data() const [[clang::lifetimebound]] { return scratch; }
};

void own_storage_outlives_view(string_view text [[clang::noescape]]) {
  const char *p = nullptr;
  {
    Normalized n{text, {}};
    p = n.data(); // expected-warning {{local variable 'n' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = p[0]; // expected-note {{later used here}}
}

// A name NOT on the list was always handled correctly -- the borrow is of the
// object -- which is what localized the defect to the name heuristic.
struct [[gsl::Pointer(char)]] NamedOther {
  string_view src;
  char scratch[64];
  const char *peek() const [[clang::lifetimebound]] { return scratch; }
};

void unlisted_name(string_view text [[clang::noescape]]) {
  const char *p = nullptr;
  {
    NamedOther n{text, {}};
    p = n.peek(); // expected-warning {{local variable 'n' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = p[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Without an annotation the heuristic still applies, so a view that forwards its
// pointee keeps being tracked against the VIEWED object.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Forwards {
  string_view src;
  // expected-warning@+1 {{member function returning 'const char *' is not annotated}}
  const char *data() const { return src.data(); } // no annotation: guess applies
};

void viewed_object_dies() {
  const char *p = nullptr;
  {
    string s = "0123456789012345678901234567890123456789";
    Forwards f{s}; // expected-warning {{local variable 's' does not live long enough}}
    p = f.data();
  } // expected-note {{destroyed here}}
  sink = p[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// The standard library is unaffected: none of its views annotate these accessors,
// so the borrow is still attributed to the VIEWED object, not to the view.
//===----------------------------------------------------------------------===//

void stl_view_still_tracks_viewed() {
  const char *p = nullptr;
  {
    string s = "0123456789012345678901234567890123456789";
    string_view sv = s; // expected-warning {{local variable 's' does not live long enough}}
    p = sv.data();
  } // expected-note {{destroyed here}}
  sink = p[0]; // expected-note {{later used here}}
}

void stl_iterator_still_tracks_viewed() {
  const char *p = nullptr;
  {
    string s = "0123456789012345678901234567890123456789";
    string_view sv = s; // expected-warning {{local variable 's' does not live long enough}}
    p = sv.begin();
  } // expected-note {{destroyed here}}
  sink = *p; // expected-note {{later used here}}
}

// Negative: a view used while what it views is alive is fine.
void stl_view_alive() {
  string s = "0123456789012345678901234567890123456789";
  string_view sv = s;
  const char *p = sv.data();
  sink = p[0]; // no-warning
}
