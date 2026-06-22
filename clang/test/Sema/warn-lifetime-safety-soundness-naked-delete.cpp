// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-naked-delete -verify %s

__attribute__((malloc)) void *my_alloc(unsigned);
namespace std {
template <class T> void destroy_at(T *);
}

// Allocations the analysis tracks: new, new[], and malloc-attributed calls.
// Deallocating them is fine.
void tracked_new() {
  int *p = new int;
  delete p; // no-warning
}

void array_new() {
  int *p = new int[4];
  delete[] p; // no-warning
}

void tracked_malloc() {
  int *p = (int *)my_alloc(4);
  std::destroy_at(p); // no-warning
  (void)p;
}

// Strict: if any loan flowing into the deallocated pointer is not a heap
// allocation, the analysis cannot prove the deallocation is valid.
void naked_delete_stack() {
  int x;
  int *p = &x;
  delete p; // expected-warning {{deleting a pointer whose allocation was not seen by lifetime safety analysis; it cannot be verified to be a live, unaliased heap allocation}}
}

void naked_destroy_stack() {
  int x;
  int *p = &x;
  std::destroy_at(p); // expected-warning {{freeing a pointer whose allocation was not seen by lifetime safety analysis}}
  (void)p;
}

// A pointer that may flow from a non-heap source on some path is still naked.
void naked_mixed(bool c) {
  int x;
  int *p = new int;
  if (c)
    p = &x;
  delete p; // expected-warning {{deleting a pointer whose allocation was not seen by lifetime safety analysis}}
}

// Deallocations inside a destructor are exempt: freeing owned members there is
// the normal ownership pattern.
struct Owner {
  int *p;
  ~Owner() { delete p; } // no-warning
};

// An untracked pointer (empty loan set -- the allocation was never seen) is
// also naked: a const member function deleting through a pointer member needs
// neither 'mutable' nor 'const_cast', yet it invalidates borrows.
struct Cache {
  int *data;
  void clear() const {
    delete[] data; // expected-warning {{deleting a pointer whose allocation was not seen by lifetime safety analysis}}
  }
};

// The destructor exemption does NOT apply to a [[gsl::Pointer]] view: a view
// owns nothing, so its destructor must not deallocate. A freeing view-destructor
// is a contract lie -- a borrow handed into the view (e.g. by aggregate
// initialization, which has no constructor parameter to flag) is silently turned
// into a dangling alias the caller cannot see. Verify the body: the `delete` of
// the (untracked) member pointer is naked.
struct [[gsl::Pointer(int)]] BadView {
  int *p;
  ~BadView() { delete p; } // expected-warning {{deleting a pointer whose allocation was not seen by lifetime safety analysis}}
};

struct [[gsl::Pointer]] BadViewNoArg {
  int *p;
  ~BadViewNoArg() { delete[] p; } // expected-warning {{deleting a pointer whose allocation was not seen by lifetime safety analysis}}
};

// A [[gsl::Owner]]'s destructor is still exempt -- an owner is expected to free
// the storage it owns.
struct [[gsl::Owner(int)]] GoodOwner {
  int *p;
  ~GoodOwner() { delete p; } // no-warning
};

// A view with a trivial (non-freeing) destructor is fine.
struct [[gsl::Pointer(int)]] GoodView {
  int *p;
};
int sink_gv(GoodView v) { return *v.p; } // no-warning

// A [[gsl::Pointer]] view's destructor is its OWN dtor body, but destroying it
// also runs its base and member subobject destructors -- which may live in
// another TU. A view must not deallocate when destroyed, so each subobject's
// destructor must provably not deallocate: be trivial, be a [[gsl::Owner]] /
// [[gsl::Pointer]] (an owner is meant to free; a view is itself verified), or
// have a visible body that does not deallocate. Checked recursively at the view's
// definition (cross-TU-sound).

struct FreerBase {
  int *p;
  ~FreerBase() { delete p; } // freeing base, view-ness on the derived
};
struct [[gsl::Pointer(int)]] ViewFreeBase // expected-warning {{base class 'FreerBase' of [[gsl::Pointer]] 'ViewFreeBase' may deallocate in its destructor}}
    : FreerBase {};

// The base's destructor body is not visible here -- conservatively rejected.
struct UnseenDtorBase {
  int *p;
  ~UnseenDtorBase();
};
struct [[gsl::Pointer(int)]] ViewUnseenBase // expected-warning {{base class 'UnseenDtorBase' of [[gsl::Pointer]] 'ViewUnseenBase' may deallocate in its destructor}}
    : UnseenDtorBase {};

// Indirect freeing base (through a plain intermediate): still caught, recursively.
struct PlainMid : FreerBase {};
struct [[gsl::Pointer(int)]] ViewIndirect // expected-warning {{base class 'PlainMid' of [[gsl::Pointer]] 'ViewIndirect' may deallocate in its destructor}}
    : PlainMid {};

// A freeing non-owner MEMBER of a base is caught too.
struct Freer {
  int *p;
  ~Freer() { delete p; }
};
struct BaseWithFreerMember {
  Freer f;
};
struct [[gsl::Pointer(int)]] ViewFreerMember // expected-warning {{base class 'BaseWithFreerMember' of [[gsl::Pointer]] 'ViewFreerMember' may deallocate in its destructor}}
    : BaseWithFreerMember {};

// Negatives.

// A trivial base frees nothing.
struct TrivialBase {
  int *p;
};
struct [[gsl::Pointer(int)]] ViewTrivialBase : TrivialBase {}; // no-warning

// A base whose visible destructor body does not deallocate.
struct SafeBase {
  int *p;
  int n;
  ~SafeBase() { n = 0; }
};
struct [[gsl::Pointer(int)]] ViewSafeBase : SafeBase {}; // no-warning

// A base that is itself a [[gsl::Pointer]] -- independently verified.
struct [[gsl::Pointer(int)]] ViewBase {
  int *p;
  ~ViewBase() {}
};
struct [[gsl::Pointer(int)]] ViewOfView : ViewBase {}; // no-warning

// A base that owns the storage it frees ([[gsl::Owner]]) -- freeing is its job;
// the same holds for a freeing gsl::Owner member of a base.
struct [[gsl::Owner(int)]] OwnerBase {
  int *p;
  ~OwnerBase() { delete p; }
};
struct [[gsl::Pointer(int)]] ViewOwnerBase : OwnerBase {}; // no-warning

struct BaseWithOwnerMember {
  OwnerBase o;
};
struct [[gsl::Pointer(int)]] ViewOwnerMember : BaseWithOwnerMember {}; // no-warning

// A non-view derived class with a freeing base is fine (not a view contract).
struct PlainDerived : FreerBase {}; // no-warning

// The same rule applies to a view's direct MEMBER subobjects, not just bases:
// destroying the view runs each member's destructor too.

// A freeing member (directly, and nested through a plain wrapper).
struct [[gsl::Pointer(int)]] ViewFreerMemberDirect {
  Freer f; // expected-warning {{member 'Freer' of [[gsl::Pointer]] 'ViewFreerMemberDirect' may deallocate in its destructor}}
  int *v;
};
struct WrapFreer {
  Freer f;
};
struct [[gsl::Pointer(int)]] ViewFreerMemberNested {
  WrapFreer w; // expected-warning {{member 'WrapFreer' of [[gsl::Pointer]] 'ViewFreerMemberNested' may deallocate in its destructor}}
  int *v;
};
// Member whose destructor body is not visible -- conservatively rejected.
struct UnseenFreer {
  int *p;
  ~UnseenFreer();
};
struct [[gsl::Pointer(int)]] ViewUnseenMember {
  UnseenFreer f; // expected-warning {{member 'UnseenFreer' of [[gsl::Pointer]] 'ViewUnseenMember' may deallocate in its destructor}}
};

// Negatives for the member path.
struct [[gsl::Pointer(int)]] ViewOwnerFieldMember {
  OwnerBase o; // no-warning (a gsl::Owner member frees what it owns)
  int *v;
};
struct [[gsl::Pointer(int)]] ViewSafeMember {
  SafeBase s; // no-warning (visible non-deallocating destructor)
  int *v;
};
struct [[gsl::Pointer(int)]] ViewTrivialMember {
  TrivialBase t; // no-warning
  int *v;
};
