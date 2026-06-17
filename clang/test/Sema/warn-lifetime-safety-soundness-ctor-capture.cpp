// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-ctor-capture -verify %s

// 'lifetime_capture_by(this)' on a *constructor* is rejected: the captured
// borrow lands on a member origin where a sibling member's valid loan can
// silently mask the dangling one (the constructor capture is not modeled). The
// relationship "the constructed object may refer to this parameter" is exactly
// what [[clang::lifetimebound]] expresses, and a lifetimebound constructor
// parameter *is* tracked, so the construct is redirected to it.

struct [[gsl::Owner(int)]] Box {
  int v;
  const int *data() const [[clang::lifetimebound]] { return &v; }
};

struct [[gsl::Pointer(int)]] View {
  const int *p;
  const int &extra;
  View(const Box &b [[clang::lifetimebound]],
       const int &e [[clang::lifetime_capture_by(this)]]) // expected-warning {{'lifetime_capture_by(this)' on a constructor is not supported by lifetime safety; annotate the parameter with '[[clang::lifetimebound]]' instead}}
      : p(b.data()), extra(e) {}
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// The recommended spelling: a [[clang::lifetimebound]] constructor parameter is
// tracked precisely and is not flagged here.
struct [[gsl::Pointer(int)]] ViewLB {
  const int &extra;
  ViewLB(const int &e [[clang::lifetimebound]]) : extra(e) {} // no-warning
};

// 'lifetime_capture_by(this)' on a non-constructor member function is the
// ordinary capture case and stays allowed (for a view receiver).
struct [[gsl::Pointer(int)]] Setter {
  const int *p;
  void set(const int &e [[clang::lifetime_capture_by(this)]]); // no-warning
};

// Capturing into a named parameter (not 'this') from a constructor is a
// different relationship and is not banned.
struct Sink {
  const int *p;
};
struct Maker {
  Maker(Sink &out, const int &e [[clang::lifetime_capture_by(out)]]); // no-warning
};
