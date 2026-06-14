// DESC: a C++23 deducing-this method's explicit (non-object) parameter `owner`
// is reallocated inside the method (`owner.push_back`). The argument index ->
// parameter mapping in the invalidation helper used the implicit-`this`
// off-by-one, matching `owner` against the explicit object parameter `self`, so
// the assumed-invalidation of the live view into `owner` was missed. Both params
// are [[clang::noescape]], which also silences the unannotated-indirection
// fallback. Fixed by a shared explicit-object-aware arg->param mapping.
// FLAGS: -std=c++23
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Mutator {
  int n = 0;
  void grow(this Mutator &self [[clang::noescape]],
            std::string &owner [[clang::noescape]]) {
    self.n++;
    owner.append(5000, 'q'); // reallocates owner
  }
};

int main() {
  Mutator m;
  std::string s(60, 'a');
  std::string_view v = s; // borrow into s
  m.grow(s);              // reallocates s -> invalidates v
  volatile char c = v.empty() ? 0 : v[0]; // use-after-free
  return c;
}
