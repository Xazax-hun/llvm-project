// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A [[gsl::Pointer]] object with a non-trivial destructor that reads its
// captured borrow is destroyed in reverse construction order. If it is declared
// BEFORE the local it borrows, that local (destroyed first) is already gone when
// the destructor runs -> a use-after-scope inside the destructor.
//
// The CFG builder previously batched a trivial-type local's lifetime-end AFTER
// all non-trivial destructors in the scope, so the borrowed-from local appeared
// to still be alive at the view's destruction; the bug was silent. The CFG now
// interleaves trivial and non-trivial cleanups in reverse construction order.

struct [[gsl::Pointer(int)]] View {
  const int *p;
  ~View(); // non-trivial: may read *p
};

void declared_before() {
  View v{nullptr}; // declared first -> destroyed LAST
  int a = 7;
  v = View{&a}; // expected-warning {{local variable 'a' does not live long enough}}
} // expected-note 2 {{}}

void several_locals() {
  View v{nullptr};
  int a = 1;
  int b = 2;
  (void)a;
  v = View{&b}; // expected-warning {{local variable 'b' does not live long enough}}
} // expected-note 2 {{}}

// Negative: the view declared AFTER the local is destroyed FIRST, so ~View()
// reads a still-live 'a' -- no error.
void declared_after() {
  int a = 7;
  View v{nullptr};
  v = View{&a}; // no-warning
}
