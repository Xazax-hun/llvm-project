// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A [[gsl::Owner]] is expected to encapsulate the resource it owns, and the
// analysis treats an owner's contents as opaque (a leaf in the origin tree). A
// *public* data member that can hold a borrow (a raw pointer, reference, or
// [[gsl::Pointer]] view) breaks that abstraction: external code can store a
// borrow into it that the analysis cannot track, so it can dangle silently --
// the type is not a sound owner. Such members are flagged at the definition.
//
// This closes a bypass: a [[gsl::Pointer]] view holding such an owner as a
// member (`struct [[gsl::Pointer]] V { Box box; }`, `Box` a gsl::Owner with a
// public `int* p`) let a store `v.box.p = &local` land on a transient origin
// and be dropped, masked by a sibling member's construction loan.

struct [[gsl::Owner]] BadOwnerPtr {
  int *p; // expected-warning {{public data member 'p' of a [[gsl::Owner]] type can hold a borrow}}
};

struct [[gsl::Owner]] BadOwnerRef {
  const int &r; // expected-warning {{public data member 'r' of a [[gsl::Owner]] type can hold a borrow}}
};

struct [[gsl::Pointer]] SomeView {
  const int *v;
};
struct [[gsl::Owner]] BadOwnerView {
  SomeView view; // expected-warning {{public data member 'view' of a [[gsl::Owner]] type can hold a borrow}}
};

// A public C-array of pointers is flagged too (array dimensions are peeled).
struct [[gsl::Owner]] BadOwnerArray {
  int *arr[4]; // expected-warning {{public data member 'arr' of a [[gsl::Owner]] type can hold a borrow}}
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A PRIVATE borrow-holding member is how a real owner manages its resource.
class [[gsl::Owner]] GoodOwner {
  int *resource; // no-warning: private (encapsulated)
public:
  int *data() const;
};

// A struct owner that keeps its handle private is fine.
struct [[gsl::Owner]] GoodOwnerStruct {
private:
  char *buf; // no-warning
};

// A [[gsl::Pointer]] (view) is *meant* to hold a borrow: a public pointer member
// is the normal case and is not flagged by this check.
struct [[gsl::Pointer]] OkView {
  const int *p; // no-warning: a view holds a borrow
};

// A plain (non-owner) struct with a public pointer is handled by other checks
// (unknown-ownership), not this one.
struct PlainHolder {
  int *p; // no-warning from this check
};

// A function pointer member is not a borrow into trackable storage.
struct [[gsl::Owner]] OwnerWithFnPtr {
  void (*callback)(); // no-warning: function pointer
};

// A non-borrow member is fine.
struct [[gsl::Owner]] OwnerValue {
  int count;
  double weight;
};
