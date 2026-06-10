// DESC: a string_view borrows the std::string owned by a unique_ptr; the string
// is then mutated THROUGH the unique_ptr (`p->append`), reallocating its buffer
// and leaving the view dangling. The mutation receiver is a smart-pointer
// dereference, not a plain variable.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>
#include <cstddef>

int main() {
  auto p = std::make_unique<std::string>(50, 'a');
  std::string_view v = *p;               // v borrows the pointee's buffer
  p->append(10000, 'b');                 // reallocates *p -> v dangles
  volatile char c = v.size() ? v[0] : 0; // use-after-free
  (void)c;
  return 0;
}
