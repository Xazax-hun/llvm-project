// RUN: %clang_cc1 -fsyntax-only -verify %s
// RUN: %clang_cc1 -fsyntax-only -verify -Wlifetime-safety %s

// 'lifetime_immortal' is a declaration attribute that applies to functions and
// is written in the leading position.
[[clang::lifetime_immortal]] int *good_fn();
[[clang::lifetime_immortal]] void good_void_fn(); // Valid subject (no effect).

[[clang::lifetime_immortal]] int bad_var; // expected-error {{'clang::lifetime_immortal' attribute only applies to functions}}
struct [[clang::lifetime_immortal]] BadStruct {}; // expected-error {{'clang::lifetime_immortal' attribute only applies to functions}}
[[clang::lifetime_immortal(1)]] int *bad_args(); // expected-error {{'clang::lifetime_immortal' attribute takes no arguments}}

// The result of an immortal function never dangles: returning or using it does
// not produce lifetime-safety diagnostics.
[[clang::lifetime_immortal]] int *immortal();
void use(int *);

int *return_immortal() { return immortal(); } // no-warning
void use_immortal() {
  int *p = immortal();
  use(p); // no-warning
}
