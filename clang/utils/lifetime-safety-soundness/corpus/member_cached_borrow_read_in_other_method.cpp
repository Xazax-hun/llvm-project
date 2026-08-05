// DESC: a borrow cached in a pointer MEMBER, invalidated before the method
// returns, and read from a *different* method. Nothing in the mutating method uses
// the cached borrow, so the only fact keeping it live is the field escape at exit
// -- and the escape-caused report branches handled only two anchors (an issuing
// expression, or a placeholder parameter). The seed loan issued for a pointer
// member of `this` has neither, so the detected invalidation was silently dropped.
// The seed's access path names the member the borrow came through, which anchors
// the report; because the fields are already treated as escaped at the end of the
// method, the split store/read across methods is reachable without any
// inter-procedural reasoning.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct [[gsl::Owner]] Box {
  Box() : pv(new std::vector<int>{1, 2, 3}) {}
  ~Box() { delete pv; }

  void bad() {
    p = pv->data();    // cache a borrow into *pv in a member
    pv->push_back(99); // reallocates *pv -> p now dangles
  }                    // nothing here reads p

  int read() const { return *p; } // the dangling read is in another method

private:
  std::vector<int> *pv;
  int *p = nullptr;
};

int main() {
  Box b;
  b.bad();
  sink = b.read(); // heap-use-after-free
  return 0;
}
