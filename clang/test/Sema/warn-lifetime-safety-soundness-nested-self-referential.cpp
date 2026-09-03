// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// A view member bound to a SIBLING owner member of the same object: the view
// dangles as soon as the owner is reassigned, and the object hands the stale
// view out. The direct spelling was reported; the same store through a NESTED
// view member was not.
//
// The check compared the stored borrow against the destination container's
// loans and asked whether the borrow points INTO that container. For
// `name = body` the container is `$this`, which is a prefix of `$this.body`, so
// it matched. For `hdr.name = body` the container is `$this.hdr` and the borrow
// is `$this.body`: siblings, so neither is a prefix of the other, and nothing
// matched -- though both live in one object and the store means the same thing.
// Sharing an enclosing object is the relation that matters, which for two
// disjoint subobjects is same-root-and-diverging.

struct [[gsl::Pointer]] Header {
  std::string_view name;
};

//===----------------------------------------------------------------------===//
// The two spellings.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Direct {
private:
  std::string_view name;
  std::string body;

public:
  Direct() : body("a long heap allocated value") {
    name = body; // expected-warning {{member is bound to a sibling}}
  }
};

struct [[gsl::Owner]] Nested {
private:
  Header hdr;
  std::string body;

public:
  Nested() : body("a long heap allocated value") {
    hdr.name = body; // expected-warning {{member is bound to a sibling}}
  }
};

// The binding need not happen in a constructor.
struct [[gsl::Owner]] BoundLater {
private:
  Header hdr;
  std::string body;

public:
  BoundLater() : body("a long heap allocated value") {}
  void bind() {
    hdr.name = body; // expected-warning {{member is bound to a sibling}}
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent: the borrow does not come from a sibling of the same object.
//===----------------------------------------------------------------------===//

const char *G = "a string literal, which outlives every object";

struct [[gsl::Owner]] FromGlobal {
private:
  Header hdr;

public:
  FromGlobal() {
    hdr.name = G; // expected-warning {{cannot track global variable 'G' here}}
                  // no self-referential warning: G is not a member of this object
  }
};

struct [[gsl::Owner]] FromLiteral {
private:
  Header hdr;

public:
  FromLiteral() {
    hdr.name = "literal"; // no-warning
  }
};

// A store into a DIFFERENT object's nested view has different roots, so it does
// not diverge-match either; it is refused separately as a store the analysis
// cannot route.
struct [[gsl::Owner]] Other {
private:
  Header hdr;
  std::string body;

public:
  // `o`'s own origin carries no borrow -- the borrow lands on its member's
  // origin -- so reading it trips the lost-borrow sentinel here.
  void bind(Other &o [[clang::noescape]]) {
    // expected-warning@+2 {{assignment through this expression is not modeled}}
    // expected-warning@+1 {{lifetime safety cannot track parameter 'o' here}}
    o.hdr.name = body;
  }
};

struct [[gsl::Owner]] NoView {
private:
  std::string body;

public:
  NoView() : body("x") {}
  const std::string &get() const [[clang::lifetimebound]] { return body; }
};
