// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-owner-of-indirection -verify %s

// Under the safe programming model a [[gsl::Owner]] container whose element
// type is an indirection (a pointer, reference, or [[gsl::Pointer]]) is
// rejected: the analysis does not track borrows held by individual elements,
// so a borrow stored into an element could otherwise dangle unnoticed.

namespace std {
template <class T> struct [[gsl::Owner]] vector {
  vector();
};
template <class T, unsigned N> struct [[gsl::Owner]] array {};
template <class K, class V> struct [[gsl::Owner]] map {};
struct [[gsl::Owner]] string {};
struct [[gsl::Pointer]] string_view {};
template <class T> struct default_delete {};
template <class T, class D = default_delete<T>> struct [[gsl::Owner]] unique_ptr {
  unique_ptr();
};
} // namespace std


// Reject: element type is an indirection.
void local_vector_of_ptr() {
  std::vector<int *> v; // expected-warning {{type 'std::vector<int *>' is a container whose element type holds a borrow (a pointer, reference, or unannotated borrow-holding type); lifetime safety cannot track borrows held by its elements}}
  (void)v;
}

void local_array_of_ptr() {
  std::array<int *, 4> a; // expected-warning {{is a container whose element type holds a borrow}}
  (void)a;
}

void local_vector_of_view() {
  std::vector<std::string_view> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}

void local_map_value_ptr() {
  std::map<int, int *> m; // expected-warning {{is a container whose element type holds a borrow}}
  (void)m;
}

// Recurses into owner template arguments.
void nested() {
  std::vector<std::vector<int *>> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}

// Parameters, by value and by reference (the latter seen through the reference;
// holds regardless of any annotation since the elements are untracked).
void param_byval(std::vector<int *> v) { (void)v; } // expected-warning {{is a container whose element type holds a borrow}}
void param_byref(std::vector<int *> &v) { (void)v; } // expected-warning {{is a container whose element type holds a borrow}}

// Call returning such a container.
std::vector<int *> make();
void call_return() {
  make(); // expected-warning {{is a container whose element type holds a borrow}}
}

// Accept: element type is not an indirection.
void ok() {
  std::vector<int> a;
  std::vector<std::string> b;
  std::vector<std::vector<int>> c;
  std::unique_ptr<std::string> d;      // element is a recognized owner
  std::unique_ptr<std::vector<int>> e; // deleter arg is uninstantiated/incomplete
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
}

// An owner whose element is an UNANNOTATED user type that itself holds a borrow
// (a view/pointer member) is rejected: the element type must be annotated
// [[gsl::Owner]]/[[gsl::Pointer]] before the container can be modeled. This is
// the case even when the owner is a smart pointer reached via `operator->`.
struct ViewHolder {
  std::string_view sv;
};
void owner_of_unknown_ownership() {
  std::unique_ptr<ViewHolder> p; // expected-warning {{is a container whose element type holds a borrow}}
  (void)p;
  std::vector<ViewHolder> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}

// Annotated element types are trusted (the model does not verify an owner
// actually manages its memory), so they are NOT rejected here.
struct [[gsl::Owner]] AnnotatedOwner {
  std::string_view sv;
};
struct [[gsl::Pointer]] AnnotatedPointer {
  std::string_view sv;
};
void owner_of_annotated() {
  std::unique_ptr<AnnotatedOwner> a; // no-warning (annotated owner element)
  std::unique_ptr<AnnotatedPointer> b; // expected-warning {{is a container whose element type holds a borrow}}
  (void)a;
  (void)b;
}

// An incomplete element cannot be shown to hold a borrow (and flagging it would
// break the common pimpl idiom), so it is not rejected.
struct Incomplete;
void owner_of_incomplete() {
  std::unique_ptr<Incomplete> p; // no-warning
  (void)p;
}

// A [[gsl::Owner(T)]] whose declared owned type T (the attribute's optional type
// argument) is itself an indirection is a self-contradictory "owner of a view"
// and is rejected directly -- the attribute-argument analogue of a container of
// indirections. This also makes an owner *of* such a type (e.g.
// unique_ptr<OwnsView>) rejected via the recursive element check.
struct [[gsl::Owner(std::string_view)]] OwnsView {
  std::string_view sv;
};
struct [[gsl::Owner(int *)]] OwnsPtr {
  int *p;
};
// As a parameter (carries a placeholder loan, so no lost-loan noise under the
// umbrella) and as a container element.
void attr_owner_of_view_param(OwnsView a) { (void)a; } // expected-warning {{is a container whose element type holds a borrow}}
void attr_owner_of_ptr_param(OwnsPtr b) { (void)b; }   // expected-warning {{is a container whose element type holds a borrow}}
void attr_owner_of_indirection_element() {
  std::unique_ptr<OwnsView> c; // expected-warning {{is a container whose element type holds a borrow}}
  std::vector<OwnsPtr> d;      // expected-warning {{is a container whose element type holds a borrow}}
  (void)c;
  (void)d;
}

// A [[gsl::Owner(T)]] whose declared owned type is NOT an indirection is a
// normal owner and stays silent.
struct [[gsl::Owner(std::string)]] OwnsString {
  std::string s;
};
void attr_owner_of_value(OwnsString ok) { (void)ok; } // no-warning

// An owner-of-indirection used as a DATA MEMBER is rejected at the class
// definition, mirroring the local/parameter/element checks: a borrow stored
// through such a member (e.g. `this->views[i] = local`) is dropped and dangles
// silently otherwise.
struct HasOwnerOfIndirectionMembers {
  std::vector<std::string_view> views; // expected-warning {{is a container whose element type holds a borrow}}
  std::vector<int *> ptrs;             // expected-warning {{is a container whose element type holds a borrow}}
  std::vector<std::string> ok_strings; // no-warning
  std::vector<int> ok_ints;            // no-warning
};

// Members that are NOT owners-of-indirection stay silent.
struct OkMembers {
  std::vector<std::string> a;
  std::vector<int> b;
  std::string_view direct_view; // a direct view member is fine (tracked)
};

// Per-construct opt-out.
void opt_out() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-owner-of-indirection"
  std::vector<int *> v; // no-warning
  (void)v;
#pragma clang diagnostic pop
}
