// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-unsupported-store -verify %s

// A capture is a store, and is refused on the same terms as any other.
//
// Routing a capture by the loans its receiver holds only helps when those loans
// name storage. Where they do not, the capture used to be waved through on the
// grounds that it ALSO emits a direct flow into the receiver -- but that flow
// targets the r-value the receiver expression produced, which is a throwaway for
// anything but a plain variable. Treating it as a baseline is what let a capture
// through a member receiver vanish entirely (see
// warn-lifetime-safety-soundness-capture-into-member.cpp), so an unresolved
// capture destination is now refused like any other unresolved store.
//
// Only -Wlifetime-safety-unsupported-store is enabled here: the shapes that
// defeat the routing also trip several other soundness backstops, and this file
// is about which stores are refused, not about how else they are covered.

struct [[gsl::Pointer]] Sink {
  const int *p;
  void take(const int *q [[clang::lifetime_capture_by(this)]]) { p = q; }
};

Sink g_sink;
Sink &pick() { return g_sink; }

// The receiver is a call result: its loans do not name storage the analysis can
// write, so the capture is refused rather than silently dropped.
void capture_into_call_result() {
  int local = 0;
  pick().take(&local); // expected-warning {{assignment through this expression is not modeled}}
}

// A temporary that is not lifetime-extended dies at the end of the full
// expression, so nothing can read a borrow stored into it afterwards. There is
// no loss to report, and refusing would fire on every `Widget{}.take(x)`.
void capture_into_temporary(const int *q) {
  Sink{}.take(q); // no-warning
}

// An EXTENDED temporary does outlive the statement, so it is not exempt.
void capture_into_extended_temporary(const int *q) {
  Sink &&s = Sink{};
  s.take(q); // expected-warning {{assignment through this expression is not modeled}}
}

// A plain variable receiver resolves to storage, so it is tracked, not refused.
void capture_into_local(const int *q) {
  Sink s;
  s.take(q); // no-warning
}

// A member receiver resolves too -- that is the case the routing was extended
// for -- so it must not regress into a refusal.
struct [[gsl::Pointer]] Holder { Sink s; };

void capture_into_member(const int *q) {
  Holder h;
  h.s.take(q); // no-warning
}

// `this` as the receiver resolves to the implicit-object placeholder.
struct [[gsl::Pointer]] SelfCapture {
  Sink s;
  void stash(const int *q) { s.take(q); } // no-warning
};
