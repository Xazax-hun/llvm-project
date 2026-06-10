// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

// A [[lifetimebound]]-this accessor returns a view bound to the whole object --
// the analysis no longer knows which subobject the view borrows. So mutating
// ANY owner field of that object must conservatively invalidate the view. A
// view that *directly* borrowed a specific field keeps precise (per-field)
// behavior; a raw pointer that points *at* the object (not into it) is not
// invalidated by a field mutation.

struct Doc {
  string content;
  string_view getView() [[clang::lifetimebound]] { return string_view(content); }
};

// Object-bound view, field mutated directly.
int direct_field_mutation() {
  Doc d;
  string_view v = d.getView(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.content += "x";              // expected-note {{invalidated here}}
  return v.size();             // expected-note {{later used here}}
}

struct PtrDoc {
  unique_ptr<string> content;
  string_view getView() [[clang::lifetimebound]] { return string_view(*content); }
};

// Object-bound view, the owning indirection reset (frees the pointee).
int unique_ptr_reset() {
  PtrDoc d;
  string_view v = d.getView(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.content.reset();           // expected-note {{invalidated here}}
  return v.size();             // expected-note {{later used here}}
}

// Object-bound view, mutate the pointee through the pointer.
int unique_ptr_mutate() {
  PtrDoc d;
  string_view v = d.getView(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.content->clear();          // expected-note {{invalidated here}}
  return v.size();             // expected-note {{later used here}}
}

// The object-bound borrow may be a RAW pointer (not a gsl::Pointer view): a
// lifetimebound accessor returning `const char*` into a member's buffer.
struct RawDoc {
  string content;
  const char *data() const [[clang::lifetimebound]] { return content.c_str(); }
};
int raw_pointer_accessor() {
  RawDoc d;
  const char *p = d.data(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.content += "x";         // expected-note {{invalidated here}}
  return p[0];              // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// A loan to a subobject is invalidated when the parent object is mutated (the
// borrow carries the parent's loan).
//===----------------------------------------------------------------------===//

struct Container {
  vector<int> items;
  void rebuild(); // non-const: may mutate items
};
int subobject_parent_mutation() {
  Container c;
  auto it = c.items.begin(); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  c.rebuild();               // expected-note {{assumed to be invalidated by this operation}}
  return *it;
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

struct TwoFields {
  vector<int> a;
  vector<int> b;
};

// A view that directly borrowed one field is NOT invalidated by mutating the
// sibling field (precise per-field info is kept).
int sibling_is_precise() {
  TwoFields s;
  auto it = s.a.begin();
  s.b.push_back(1); // no-warning (different field)
  return *it;
}

// A raw pointer that points AT the object is not a view into it; a field
// mutation does not invalidate it.
int pointer_at_object() {
  Doc d;
  Doc *p = &d;
  d.content += "x"; // no-warning (object storage is unaffected)
  return p->content.c_str() != nullptr;
}
