// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-owner-capture \
// RUN:   -Wlifetime-safety-owner-public-pointer -verify %s

// A [[gsl::Owner]]'s encapsulation checks (a public borrow-holding member, and a
// lifetime_capture_by(this) setter) must include members/methods inherited from
// a non-owner base -- otherwise the capture machinery can hide in the base while
// the derived type is trusted as an opaque owner.

struct CaptureBase {
  const char *hidden = nullptr; // expected-warning {{public data member 'hidden' of a [[gsl::Owner]] type can hold a borrow}}
  void stash(const char *p [[clang::lifetime_capture_by(this)]]) { // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
    hidden = p;
  }
};
struct [[gsl::Owner(char)]] InheritsCapture : CaptureBase {};

// Inherited through an intermediate non-owner base (two levels up) is caught.
struct DeepBase {
  int *dp = nullptr; // expected-warning {{public data member 'dp' of a [[gsl::Owner]] type can hold a borrow}}
};
struct DeepMid : DeepBase {};
struct [[gsl::Owner(int)]] InheritsDeep : DeepMid {};

// Control: a base that is itself a [[gsl::Owner]] reports its own members at its
// own definition and must NOT be re-reported when a derived owner completes.
struct [[gsl::Owner(char)]] OwnerBase {
  char *p = nullptr; // expected-warning {{public data member 'p' of a [[gsl::Owner]] type can hold a borrow}}
};
struct [[gsl::Owner(char)]] DerivedOwner : OwnerBase {}; // no re-report of 'p'

// Control: a non-owner deriving the same base is not subject to the owner checks.
struct PlainDerived : CaptureBase {}; // no-warning

//===----------------------------------------------------------------------===//
// A member function TEMPLATE makes the same promise and is honored the same way,
// so the inherited walk must enumerate it too. RD->methods() does not list a
// FunctionTemplateDecl, so being written as a template let this through while the
// plain spelling right beside it was refused. `cached` is protected rather than
// public so the owner-public-pointer refusal does not cover for it.
//===----------------------------------------------------------------------===//

struct TmplBase {
protected:
  const char *cached = nullptr;

public:
  // Plain spelling: refused all along.
  void setPlain(const char *p [[clang::lifetime_capture_by(this)]]) { // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
    cached = p;
  }
  // Template spelling: same promise, and it was silent.
  template <class T>
  void setTmpl(T, const char *p [[clang::lifetime_capture_by(this)]]) { // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
    cached = p;
  }
};

struct [[gsl::Owner(char)]] TmplOwner : TmplBase {};

// A base no owner inherits has nothing to refuse.
struct UnusedBase {
  const char *q = nullptr;
  template <class T>
  void set(T, const char *p [[clang::lifetime_capture_by(this)]]) { // no-warning
    q = p;
  }
};
