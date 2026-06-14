// DESC: a C-style deallocation `free(p)` frees a heap block while a tracked
// alias still points into it; the alias is then read. free()/realloc()/a direct
// ::operator delete() call were not modeled as deallocations (only the C++
// `delete` expression, destructors, and std::destroy_at were), so the alias's
// loan was never invalidated. malloc is a tracked heap loan (malloc attribute);
// free's only backstop (unannotated-indirection on its void*) is silenced by a
// truthful [[clang::noescape]]. Now free/realloc/operator-delete calls model the
// deallocation, so the use-after-free is flagged.
// EXPECT-ASAN: heap-use-after-free
extern "C" __attribute__((malloc)) void *malloc(unsigned long) noexcept;
extern "C" void free(void *p [[clang::noescape]]) noexcept;

int main() {
  char *buf = (char *)malloc(64);
  for (int i = 0; i < 40; ++i)
    buf[i] = char('a' + (i % 26));
  buf[40] = 0;
  char *alias = buf; // tracked alias of the heap block
  free(buf);         // frees the block
  int sum = 0;
  for (int i = 0; i < 41; ++i)
    sum += alias[i]; // heap-use-after-free
  return sum & 1;
}
