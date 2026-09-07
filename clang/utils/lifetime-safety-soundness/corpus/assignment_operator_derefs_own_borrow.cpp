// DESC: an assignment operator of a view WRITES THROUGH the borrow the object already holds
// instead of replacing it. `r = 'x'` on a `Ref` whose `operator=(char)` does `*p = c` performs a
// write into the borrowed buffer -- but an assignment is modeled as kill-then-propagate, which
// assumes the operator only RESEATS. The model therefore discards exactly the part that can
// dangle: it kills the loan the dereference writes to, and records a WRITE of the object where
// the body performs a READ of it. So an invalidation before the assignment went unreported,
// while the identical access spelled as an ordinary method call was caught -- the two differ
// only in which member function is named.
//
// The construct is now REFUSED, asked of the LOANS rather than of the body's syntax: a member's
// origin is seeded at entry with an Uninitialized loan naming that field, so a USE of an origin
// carrying such a loan is precisely "this operator reads the borrow the object already holds",
// however the dereference is written (`*p`, `p[i]`, `p->f`). A pure reseat never uses one -- it
// reads the RIGHT operand's borrow, rooted at that parameter -- so `p = o.p` still both
// propagates and kills, and comparing or null-checking the old pointer stays silent.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

class [[gsl::Pointer(char)]] Ref {
  char *p = nullptr;

public:
  explicit Ref(char *q [[clang::lifetimebound]]) : p(q) {}
  // Reads as a convenient setter: assign a character through the reference.
  void operator=(char c) {
    if (p) {
      sink = *p; // read through the borrow first: a clean use-after-free
      *p = c;
    }
  }
};

int main() {
  std::string s(64, 'a'); // long enough to be heap-allocated, not SSO
  Ref r(s.data());       // borrows the buffer
  s.append(4000, 'z');   // reallocates, freeing it
  r = 'x';               // writes through the dangling borrow
  return 0;
}
