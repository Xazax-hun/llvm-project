// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-lifetimebound-violation -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -verify=quiet %s

// A virtual override must not strengthen its lifetime contract beyond what the
// overridden method advertises: callers dispatch through the base-class
// signature, so a '[[clang::lifetimebound]]' the override *adds* (on a parameter
// or binding the return to the implicit object) is invisible to them, and the
// returned borrow could outlive its referent undetected. The override may
// *drop* lifetimebound (that is only looser for callers). Off by default; the
// 'quiet' run expects no diagnostics.

// quiet-no-diagnostics

struct Base {
  int m;
  virtual const int *param(const int *p);                       // p NOT lifetimebound \
                                                                 // expected-note {{overridden virtual function is here}}
  virtual const int *both(const int *p [[clang::lifetimebound]]);
  virtual const int *self() const;                              // return NOT bound to this \
                                                                 // expected-note {{overridden virtual function is here}}
  virtual const int *immortal() const [[clang::lifetimebound]]; // already bound to this
};

struct Derived : Base {
  // Adds lifetimebound on a parameter the base lacks -> rejected.
  const int *param(const int *p [[clang::lifetimebound]]) override; // expected-warning {{overriding parameter 'p' adds '[[clang::lifetimebound]]' not present on the overridden method}}

  // Matches the base -> fine.
  const int *both(const int *p [[clang::lifetimebound]]) override; // no-warning

  // Adds return-binding to the implicit object the base lacks -> rejected.
  const int *self() const [[clang::lifetimebound]] override; // expected-warning {{this overriding member function binds its return value to the object}}

  // Drops the base's lifetimebound -> looser, allowed.
  const int *immortal() const override; // no-warning
};

// Overriding a 'lifetime_immortal' method (its return never dangles) requires
// the override to preserve that guarantee: it must also be 'lifetime_immortal'.
// A 'lifetimebound' override is a weaker (object-lifetime) guarantee and is
// reported as adding an unenforced binding; an unannotated override is reported
// as dropping the guarantee entirely.
struct ImmortalBase {
  [[clang::lifetime_immortal]] virtual const int *imm() const; // \
    expected-note 2 {{overridden virtual function is here}}
};

struct ImmortalDerivedOK : ImmortalBase {
  [[clang::lifetime_immortal]] const int *imm() const override; // no-warning
};

struct ImmortalDerivedUnannotated : ImmortalBase {
  const int *imm() const override; // expected-warning {{this override of a 'lifetime_immortal' method does not guarantee its return never dangles}}
};

struct ImmortalDerivedLifetimebound : ImmortalBase {
  const int *imm() const [[clang::lifetimebound]] override; // expected-warning {{this override of a 'lifetime_immortal' method does not guarantee its return never dangles}}
};
