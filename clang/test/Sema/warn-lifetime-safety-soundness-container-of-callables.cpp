// RUN: %clang_cc1 -fsyntax-only -std=c++20 -isystem %S/Inputs -Wlifetime-safety-soundness -verify %s

// A callable wrapper (std::function) or a lambda can capture a borrow, which the
// analysis cannot track per element (the capture is type-erased). A container of
// such callables (std::vector<std::function<...>>) was therefore not recognized
// as a container-of-indirection -- unlike std::vector<std::string_view> -- so a
// closure capturing a borrow, stored into an element via a braced init-list or a
// factory return (neither of which has a callable parameter to flag), was
// dropped silently. A callable-wrapper/lambda element now counts as an
// indirection, like a view element.
//
// The header is included as a system header (like real STL) so only diagnostics
// in this file surface; the function specialization is explicitly instantiated
// to be a complete type (a closure conversion does this in real code -- an
// incomplete element is conservatively not treated as an indirection).

#include <lifetime-analysis.h>
using std::function;
using std::vector;

template class std::function<int()>;

vector<function<int()>> make_local() {
  vector<function<int()>> v; // expected-warning {{is a container whose element type holds a borrow}}
  return v;
}

void take(vector<function<int()>> v) { (void)v; } // expected-warning {{is a container whose element type holds a borrow}}

// As a data member, rejected at the record definition too.
struct Holder {
  vector<function<int()>> cbs; // expected-warning {{is a container whose element type holds a borrow}}
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A container of non-borrow values is fine.
vector<int> ints() {
  vector<int> v; // no-warning
  return v;
}
