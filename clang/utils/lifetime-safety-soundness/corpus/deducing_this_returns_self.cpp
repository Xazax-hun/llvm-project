// DESC: a C++23 deducing-this method returns the object itself, with
// [[clang::lifetimebound]] on the explicit object parameter `self`. A borrow
// into the result (`&w.id().member`) is taken inline and outlives the object.
// The lifetimebound param->return flow must treat the explicit object parameter
// like `this`; otherwise the borrow is dropped and the dangling read is silent.
// FLAGS: -std=c++23
// EXPECT-ASAN: stack-use-after-scope
#include <string>

struct Wrapper {
  std::string member{"a long heap string exceeding the sso buffer size limit!!"};
  const Wrapper &id(this const Wrapper &self [[clang::lifetimebound]]) {
    return self;
  }
};

int main() {
  const std::string *p = nullptr;
  {
    Wrapper w;
    p = &w.id().member; // p borrows into w.member via the deducing-this call
  }                     // w expires here
  volatile char c = p->empty() ? 0 : (*p)[0]; // use-after-scope
  return c;
}
