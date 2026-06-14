// DESC: reallocf (a BSD/macOS libc deallocator) frees its first argument when
// the new size is 0, but was not recognized as a deallocation. Its pointer
// parameter is (correctly) [[clang::noescape]] -- a deallocator does not retain
// the pointer -- which silenced the only backstop (unannotated-indirection) for
// an unrecognized deallocator. malloc is a tracked heap loan, so the alias is a
// real borrow. reallocf/cfree/__builtin_operator_delete are now modeled as
// deallocations.
// EXPECT-ASAN: heap-use-after-free
#include <cstddef>

extern "C" __attribute__((malloc)) void *malloc(size_t);
extern "C" void *reallocf(void *p [[clang::noescape]], size_t);

volatile int g;

int main() {
  int *p = (int *)malloc(sizeof(int));
  *p = 42;
  int *alias = p;
  reallocf(p, 0); // frees p (size 0)
  g = *alias;     // heap-use-after-free
  return g;
}
