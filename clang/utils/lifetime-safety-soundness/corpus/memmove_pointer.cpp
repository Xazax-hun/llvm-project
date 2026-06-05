// DESC: a borrow copied to another pointer via memmove
// EXPECT-ASAN: stack-use-after-scope
#include <cstring>
int main() {
  int *out;
  {
    int x = 11;
    int *p = &x;
    std::memmove(&out, &p, sizeof(p));
  }
  return *out;
}
