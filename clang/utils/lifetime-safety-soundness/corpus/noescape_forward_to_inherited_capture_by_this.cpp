// DESC: a [[clang::noescape]] parameter is forwarded to an INHERITED method declared
// [[clang::lifetime_capture_by(this)]], which stashes it in the object. The base method is
// honest -- it declares the capture -- so the lie is in the derived method: `esc` promises the
// argument does not escape and then hands it to something that captures it. The caller trusts
// the promise, lets the vector reallocate, and reads a freed buffer through the object.
//
// The capture is routed by the loans the receiver's LVALUE holds, so it reaches the object
// whatever expression designated it. But the check that asks which object the store lands in
// read the state AFTER the capture's own origin flow had merged the payload into the
// destination origin. The payload's parameter was then sitting among the destination's loans,
// so the store looked like a self-store into that very parameter and the check stepped aside.
//
// Only a direct `this` receiver survived, and only via a second, narrower route that
// recognizes exactly that spelling; every base-class spelling -- inherited, explicitly
// qualified, and through an explicit upcast -- was silent. Emitting the store before the flow
// makes the pre-store state genuinely pre-store, and covers all of them at once. An AST-level
// peel of the implicit derived-to-base conversion would not have reached the explicit upcast.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct [[gsl::Pointer(int)]] Base {
  const int *d = nullptr;
  // Honest: it says the argument is captured into the object.
  void setD(const int *q [[clang::lifetime_capture_by(this)]]) { d = q; }
};

struct [[gsl::Pointer(int)]] Derived : Base {
  explicit Derived(const int *q [[clang::lifetimebound]]) { d = q; }
  // The lie: promises no escape, then forwards to something that captures.
  void esc(const int *q [[clang::noescape]]) { setD(q); }
};

static const int k = 7;

int main() {
  std::vector<int> v(4, 11);
  Derived dv{&k}; // seeded with an immortal borrow
  dv.esc(&v[0]);  // caller trusts noescape and keeps no reference of its own
  v.push_back(1); // reallocates, freeing the buffer `dv.d` now points into
  sink = *dv.d;
  return 0;
}
