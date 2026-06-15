// RUN: %clang_cc1 -fsyntax-only -std=c++20 -isystem %S/Inputs -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -isystem %S/Inputs -Wlifetime-safety-owner-of-indirection -Wlifetime-safety-pointer-of-indirection -verify %s

// A borrow-holding container/view of owner-/pointer-of-indirection type can be
// buried in the template arguments of a *non-owner aggregate* (std::pair,
// std::tuple) declared in a system header. The aggregate's own
// field-declaration check (on its `first`/`second`/element members) is
// suppressed there, and the aggregate is itself neither a [[gsl::Owner]] nor a
// [[gsl::Pointer]], so the per-record field walk would otherwise miss it. The
// safe programming model searches the template arguments of such a field type
// so the offending element/pointee is still rejected at the enclosing record's
// definition (user code).

#include <lifetime-safety-aggregates.h>

struct HolderPair {
  std::pair<std::vector<std::string_view>, int> p; // expected-warning {{type 'std::vector<std::string_view>' is a container whose element type holds a borrow}}
};

struct HolderPairPtr {
  std::pair<int, std::vector<int *>> p; // expected-warning {{type 'std::vector<int *>' is a container whose element type holds a borrow}}
};

struct HolderTupleView {
  std::tuple<int, std::span<int *>> t; // expected-warning {{type 'std::span<int *>' is a view whose pointee type holds a borrow}}
};

// Nested aggregates: the bad element is two aggregate levels down.
struct HolderNested {
  std::pair<int, std::pair<std::vector<std::string_view>, int>> p; // expected-warning {{type 'std::vector<std::string_view>' is a container whose element type holds a borrow}}
};

// A bare pair/tuple *local* of the same type (not a member) is also reached:
// the field walk only fires for records, but a local declaration is checked by
// the analysis's unknown-ownership/of-indirection machinery.
void local_ok_does_not_crash() {
  std::pair<std::string, int> ok; // no-warning: owner of char
  (void)ok;
}

// Aggregates whose arguments are all fine stay silent.
struct OkAggregates {
  std::pair<std::string, int> a;          // owner of char
  std::pair<std::unique_ptr<int>, int> b; // unique_ptr<int>
  std::tuple<int, double> c;              // no indirection
  std::pair<int, std::string_view> d;     // a direct view element is fine
};
