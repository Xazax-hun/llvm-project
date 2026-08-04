// DESC: a [[clang::lifetime_immortal]] accessor returns a `new` allocation that
// the object's own destructor frees. `lifetime_immortal` is otherwise trusted
// unverified, and the body verifier accepted any heap-rooted loan
// (AccessPath::Kind::NewAllocation) as "provably immortal" -- but heap storage
// lives only until someone frees it, and here the Arena's destructor does. An
// immortal loan never expires and is never invalidated, so the attribute silenced
// every downstream check and the use-after-free was reported by nothing.
// Worse, the model steers you here: without the attribute,
// -Wlifetime-safety-unannotated-indirection asks for either 'lifetimebound' on
// the implicit object or 'lifetime_immortal' on the function, offering the unsound
// option as a co-equal alternative.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

class [[gsl::Owner(int)]] Arena {
  int *p = nullptr;

public:
  Arena() = default;
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  ~Arena() { delete p; } // frees what alloc() handed out

  // Lies: the result lives only as long as *this, not forever.
  [[clang::lifetime_immortal]] int *alloc() {
    int *n = new int(5);
    p = n;
    return n;
  }
};

int main() {
  int *q;
  {
    Arena a;
    q = a.alloc();
  }           // ~Arena frees the allocation
  sink = *q;  // heap-use-after-free
  return 0;
}
