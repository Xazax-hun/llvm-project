// DESC: a std::span<int*> is a [[gsl::Pointer]] view modeled as a single origin
// covering only the outer array level; the pointer-elements' pointees are a
// second indirection level the analysis does not track. An element is repointed
// at a heap object which is then freed, and the dangling pointee is read back
// through the span. The multi-level-indirection ban applies to declarations
// like int**, and owner-of-indirection applies to gsl::Owner containers like
// vector<int*>, but neither covered a gsl::Pointer-of-pointers. Found by the 3rd
// multi-agent bypass hunt (B2).
// EXPECT-ASAN: heap-use-after-free
#include <span>

int main() {
  int a = 1, b = 2;
  int *arr[2] = {&a, &b};
  std::span<int *> s(arr); // view over the array of pointers
  int *heap = new int(31337);
  arr[0] = heap; // element now points at the heap object
  delete heap;   // free the pointee
  volatile int v = *s[0]; // read the freed pointee, only through the span
  (void)v;
  return 0;
}
