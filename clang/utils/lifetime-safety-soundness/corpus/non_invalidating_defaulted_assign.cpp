// DESC: '[[clang::lifetime_non_invalidating]]' on a DEFAULTED copy-assignment.
// The attribute suppresses the assumed-invalidation fact at every call site, so
// nothing but the body verifier stands between an untrue promise and a silently
// dangling borrow in the caller -- and that verifier walks a BODY. A defaulted
// method has none in the AST, so the promise was accepted for free, while the
// implicit body assigns the owner member and frees the buffer the caller's
// borrow points at. Deleting the attribute makes the same TU report the
// invalidation, which is the shape worth watching for: an annotation that
// REMOVES a diagnostic and verifies nothing in exchange. A const method and a
// type owning nothing reallocatable keep the promise trivially and are still
// accepted; a real body, even one written out of line, is verified as before.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[gsl::Owner]] Pool {
private:
  std::string s;

public:
  explicit Pool(int n) : s(n, 'a') {}
  Pool(const Pool &) = default;
  // The implicit body frees s's buffer.
  [[clang::lifetime_non_invalidating]] Pool &operator=(const Pool &) = default;
  const char *get() [[clang::lifetimebound]] { return s.c_str(); }
};

int main() {
  Pool p(100);
  const char *q = p.get(); // borrow into p's heap buffer
  Pool o(200);
  p = o;      // frees the buffer q points at
  sink = *q;  // heap-use-after-free
  return 0;
}
