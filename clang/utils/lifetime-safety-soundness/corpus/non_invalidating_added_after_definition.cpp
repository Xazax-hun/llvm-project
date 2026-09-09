// DESC: [[clang::lifetime_non_invalidating]] added by a redeclaration that FOLLOWS
// the definition. A call is checked against the declaration in scope, so the promise
// suppresses the assumed invalidation at every call after it -- while the
// DEFINITION's parameter never carries the promise, so its body is never verified
// against it. Truthful nowhere, reported nowhere.
//
// The same first-declaration rule that C++ states for carries_dependency, and that
// lifetime_capture_by already follows: the promise has to be on the first
// declaration, or some call is checked against a declaration that does not make it.
// FLAGS: -Wno-lifetime-safety-unannotated-indirection
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int g_sink;

[[gnu::noinline]] void useI(const int *p [[clang::noescape]]) { g_sink = *p; }

// The definition does reallocate.
void grow(std::vector<int> &v [[clang::noescape]]) {
  for (int i = 0; i < 256; ++i)
    v.push_back(i);
}

// Adds the promise afterwards; never verified, but honoured by callers below.
void grow(std::vector<int> &v [[clang::noescape]]
                              [[clang::lifetime_non_invalidating]]);

int main() {
  std::vector<int> v{1};
  int *p = &v[0];
  grow(v);  // the assumed invalidation is suppressed by the unverified promise
  useI(p);
  return 0;
}
