// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-naked-delete -Wno-lifetime-safety-const-subversion \
// RUN:   -Wno-lifetime-safety-downcasts -Wno-lifetime-safety-unannotated-indirection \
// RUN:   -verify %s
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
  // Freeing a MEMBER here also leaves that member dangling for the destructor, which
  // runs after this function -- unlike in a destructor, where freeing what the object
  // owns is the normal pattern and nothing can read the member afterwards.
  // expected-warning@+2 {{borrow held by this member which escapes to a field is later invalidated}}
  // expected-note@+1 {{this field dangles}}
  int *owned;
  // expected-warning@+1 {{names the implicit object parameter but this function never deallocates the object}}
  void frees_something_else() __attribute__((ownership_takes(RC, 1))) {
    delete owned; // expected-note {{freed here}}
  }
  // expected-warning@+1 {{names the implicit object parameter but this function never deallocates the object}}
  void frees_nothing() __attribute__((ownership_takes(RC, 1))) { sink = 1; }
};

// A declaration with no body has nothing to verify.
struct Declared {
  void deref() __attribute__((ownership_takes(RC, 1))); // no-warning
};

//===----------------------------------------------------------------------===//
// The object is destroyed BY this call, but not IN it: the destructor still runs
// afterwards and can read a member.
//===----------------------------------------------------------------------===//

// So the field facts a DESTRUCTOR may skip -- nothing can read a member once ~T has
// returned -- must NOT be skipped here. Treating the two alike hid the release idiom
// that parks a local in a member and then has the object deleted: `~T()` reads that
// member. Only the OBJECT's own storage is exempt from the deallocation report, which
// is what the annotation licenses.
template <typename F> void onMainThread(F &&);
void logReason(const char *);

struct Misbehaved {
  mutable unsigned count{1};
  // expected-note@+1 {{this field dangles}}
  const char *deathReason{nullptr};

  ~Misbehaved() {
    if (deathReason)
      logReason(deathReason);
  }

  void deref() const __attribute__((ownership_takes(RC, 1))) {
    if (--count)
      return;
    char reason[64] = "refcount reached zero";
    // expected-warning@+1 {{stack memory associated with local variable 'reason' escapes to the field 'deathReason'}}
    const_cast<Misbehaved *>(this)->deathReason = reason;
    // Handing the object to something opaque, which may destroy it -- so the promise
    // is not reported as unkept just because the `delete this` is not visible here.
    onMainThread([this] { delete this; }); // no-warning
  }
};
