// DESC: a setjmp/longjmp back-edge: a borrow is taken into a std::string's
// buffer, the string is then grown (reallocating, freeing that buffer) and a
// longjmp transfers back to the setjmp point; on the second pass the stale
// borrow is read. The CFG does not model the longjmp->setjmp back-edge, so the
// invalidation is never connected to the read. setjmp/longjmp are rejected as
// unsupported (non-local control flow) so the model does not silently miss this.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <csetjmp>
#include <string>

std::jmp_buf jb;
volatile char sink;

int main() {
  std::string s(32, 'a');  // heap (> SSO)
  const char *p = s.c_str(); // borrow into s's buffer
  if (setjmp(jb) == 0) {
    s.append(256, 'z');      // reallocates -> frees p's buffer
    std::longjmp(jb, 1);     // jump back to setjmp
  } else {
    sink = p[0];             // use-after-free on the second pass
  }
  return sink ? 0 : 1;
}
