// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-naked-delete -Wno-lifetime-safety-const-subversion \
// RUN:   -Wno-lifetime-safety-downcasts -verify %s
//
// The naked-delete, const-subversion and downcast refusals are switched off: the
// canonical shape below trips all three (`delete const_cast<T *>(static_cast<const
// T *>(this))`), they are orthogonal refusals, and they are not what is under test.

// `__attribute__((ownership_takes(<module>, 1)))` on a non-static member function
// names the implicit object: the call deallocates the object it is called on. That
// is the intrusive reference count's `deref()`, whose body ends in `delete this`.
//
// Two consequences, matching what a destructor already means:
//
//  - At the CALL SITE the object is destroyed, so a borrow into it taken before the
//    call and used after it is reported -- exactly as for an explicit destructor
//    call or std::destroy_at.
//
//  - In the BODY the object does not outlive the call, so the implicit use of `this`
//    at exit is not emitted and field escapes are vacuous. Without that, `delete
//    this` freed the object and the implicit exit use then read it, reporting the
//    canonical `deref()` as a use-after-free.
//
// The promise is verified against the body, the way the immortal and malloc promises
// are: an annotated function that never deallocates the object would make callers
// treat a live object as destroyed, and would silence the checks that assume the
// object outlives the call.

volatile int sink;

//===----------------------------------------------------------------------===//
// The canonical shape must be accepted.
//===----------------------------------------------------------------------===//

class RefCountedBase {
public:
  void ref() const { ++m_refCount; }

protected:
  bool derefBase() const { return !--m_refCount; }

private:
  mutable unsigned m_refCount{1};
};

template <typename T> class RefCounted : public RefCountedBase {
public:
  void deref() const __attribute__((ownership_takes(RefCounted, 1))) {
    if (derefBase())
      delete const_cast<T *>(static_cast<const T *>(this));
  }

protected:
  RefCounted() = default;
  ~RefCounted() = default;
};

class Node : public RefCounted<Node> {
public:
  int m_value{0};
};

//===----------------------------------------------------------------------===//
// Reported: a borrow into the object outliving the call that destroys it.
//===----------------------------------------------------------------------===//

void borrow_used_after_deref() {
  Node *n = new Node;   // expected-warning {{object whose reference is captured is later invalidated}}
  int *p = &n->m_value;
  n->deref();           // expected-note {{invalidated here}}
  sink = *p;            // expected-note {{later used here}}
}

// The object itself, not just a borrow into it.
void object_used_after_deref() {
  Node *n = new Node; // expected-warning {{object whose reference is captured is later invalidated}}
  n->deref();         // expected-note {{invalidated here}}
  sink = n->m_value;  // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The borrow is read before the object is destroyed.
void borrow_used_before_deref() {
  Node *n = new Node;
  int *p = &n->m_value;
  sink = *p;
  n->deref(); // no-warning
}

//===----------------------------------------------------------------------===//
// The promise is verified against the body.
//===----------------------------------------------------------------------===//

struct Deletes {
  // Deallocating through the laundering casts above still counts, and so does
  // delegating to another function that takes ownership.
  void destroy() __attribute__((ownership_takes(RC, 1))) { delete this; } // no-warning
  void delegates() __attribute__((ownership_takes(RC, 1))) { destroy(); } // no-warning
  // Conditional is enough: the body may deallocate the object.
  void conditional(bool c) __attribute__((ownership_takes(RC, 1))) { // no-warning
    if (c)
      delete this;
  }
};

struct DoesNot {
  int *owned;
  // expected-warning@+1 {{names the implicit object parameter but this function never deallocates the object}}
  void frees_something_else() __attribute__((ownership_takes(RC, 1))) {
    delete owned;
  }
  // expected-warning@+1 {{names the implicit object parameter but this function never deallocates the object}}
  void frees_nothing() __attribute__((ownership_takes(RC, 1))) { sink = 1; }
};

// A declaration with no body has nothing to verify.
struct Declared {
  void deref() __attribute__((ownership_takes(RC, 1))); // no-warning
};
