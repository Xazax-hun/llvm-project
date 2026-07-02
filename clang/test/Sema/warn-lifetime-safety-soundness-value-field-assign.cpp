// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
#include "Inputs/lifetime-analysis.h"

// A value-type field member (Vec2) is assigned through its non-const copy
// assignment. The receiver `a.pos` is a plain Vec2 -- not an owner and not
// owner-containing -- so the store cannot reallocate anything and must NOT be
// flagged as an assumed invalidation, even though `a`'s loan roots (via a
// lifetimebound accessor) at an enclosing object that separately holds both an
// unrelated Vec2 subobject and a mutable owner container.
// expected-no-diagnostics

struct Vec2 { float x, y; };

struct Elem { Vec2 pos; float extra; };

template <typename T>
struct Pool {
  std::vector<T> data_;
  T &at(int i) [[clang::lifetimebound]] { return data_[i]; }
};

struct Container {
  Pool<Elem> pool_;
  Vec2 stray_; // unrelated same-typed subobject as the receiver
  void tick();
};

void Container::tick() {
  Elem &a = pool_.at(0);
  a.pos = a.pos;   // Vec2::operator= ; must not trip assumed-invalidation
  a.extra += 1.0f; // later use keeps `a` live across the store
}
