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
