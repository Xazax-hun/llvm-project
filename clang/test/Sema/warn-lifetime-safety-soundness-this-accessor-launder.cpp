// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A borrow obtained from a [[clang::lifetimebound]] accessor called on `this`
// is laundered into the `$this` placeholder loan, which carries no issuing
// expression and is not a placeholder parameter. The checker previously had no
// reportable anchor for such a loan and silently skipped it during invalidation
// checking, so a later self-mutation of the borrowed owner went unreported -- a
// silent use-after-invalidation. The borrow is now anchored at the use that
// keeps it live (the dangling read).

struct [[gsl::Owner(int)]] MyBuf {
  const int *data() const [[clang::lifetimebound]];
  void grow(); // non-const mutator (may reallocate)
};

struct W {
  MyBuf buf;
  const int *get() const [[clang::lifetimebound]] { return buf.data(); }
  void mutate() { buf.grow(); } // non-const self-method that reallocates buf

  // Assumed path: a non-const self-method call is assumed to mutate the owner.
  int bug_assumed() {
    const int *v = this->get(); // borrow laundered through `$this`
    mutate();                   // expected-note {{assumed to be invalidated by this operation}}
    return *v;                  // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  }
};

//===----------------------------------------------------------------------===//
// Negative: a const self-method between the borrow and the use cannot mutate.
//===----------------------------------------------------------------------===//
struct WOk {
  MyBuf buf;
  const int *get() const [[clang::lifetimebound]] { return buf.data(); }
  void look() const {}
  int ok() {
    const int *v = this->get();
    look(); // no-warning: const cannot mutate
    return *v;
  }
};

//===----------------------------------------------------------------------===//
// Precision: a borrow that names the field directly (carrying a precise field
// loan) is still anchored at the borrow, not the use.
//===----------------------------------------------------------------------===//
struct WDirect {
  MyBuf buf;
  void mutate() { buf.grow(); }
  int direct() {
    const int *v = buf.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    mutate();                  // expected-note {{assumed to be invalidated by this operation}}
    return *v;
  }
};
