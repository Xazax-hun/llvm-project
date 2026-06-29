// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Regression: storing a multi-level indirection (`const char**`, `int**`, or a
// pointer to another view) into a member of a [[gsl::Pointer]] leaf tripped the
// pointee-chain length-equality assertion in FactsGenerator::flow (the
// view-member-store merge flowed the depth-2 source into the view's depth-1
// pointee). The single-borrow view model cannot track such a multi-level member
// store, so it is now flagged as an unsupported store rather than crashing (or,
// in a release build, silently mis-propagating loans).

struct [[gsl::Pointer(char)]] Outer {
  const char **base; // expected-warning {{field 'base' uses more than one level of indirection}}
};

void store_multilevel_member(const char **arr) { // expected-warning {{uses more than one level of indirection}}
  Outer o;
  o.base = arr; // expected-warning {{assignment through this expression is not modeled}} \
                // expected-warning {{lifetime safety cannot track local variable 'o'}}
}

struct [[gsl::Pointer(int)]] OuterPP {
  int **base; // expected-warning {{field 'base' uses more than one level of indirection}}
};
void store_int_pp(int **arr) { // expected-warning {{uses more than one level of indirection}}
  OuterPP o;
  o.base = arr; // expected-warning {{assignment through this expression is not modeled}} \
                // expected-warning {{lifetime safety cannot track local variable 'o'}}
}

struct [[gsl::Pointer(char)]] View {
  const char *p;
};
struct [[gsl::Pointer(int)]] OuterV {
  View *base; // pointer to another view // expected-warning {{field 'base' uses more than one level of indirection}}
};
void store_view_ptr(View *arr) { // expected-warning {{uses more than one level of indirection}}
  OuterV o;
  o.base = arr; // expected-warning {{assignment through this expression is not modeled}} \
                // expected-warning {{lifetime safety cannot track local variable 'o'}}
}
