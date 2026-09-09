// DESC: the same promise lost across a plain REDECLARATION -- no virtual needed. The
// parameter attribute is an InheritableAttr (it also applies to a function) rather
// than an InheritableParamAttr, so it was not propagated to a later declaration's
// parameter. The declaration carrying it suppressed the assumed invalidation at every
// call site, while the DEFINITION's parameter never carried it, so the body was never
// verified and the untrue promise was silent at both ends.
// FLAGS: -Wno-lifetime-safety-unannotated-indirection
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int g_sink;

void tickle(std::vector<int> &v [[clang::noescape]]
                                [[clang::lifetime_non_invalidating]]);

static void run() {
  std::vector<int> v{1, 2, 3};
  int *p = &v[0];
  tickle(v); // resolves to the declaration above
  g_sink = *p;
}

// The attribute is absent here, so this body was never checked against it.
void tickle(std::vector<int> &v [[clang::noescape]]) { v.push_back(1); }

int main() {
  run();
  return 0;
}
