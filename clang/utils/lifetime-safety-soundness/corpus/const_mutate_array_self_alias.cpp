// DESC: a [[gsl::Owner]] with a PRIVATE array-of-pointers self-alias member
// (`std::string* self[1]` pointing at a sibling `buf`) let a `const` member
// function reallocate its own owner through the array element
// (`self[0]->reserve(...)`), freeing heap data a live string_view (from a
// sibling lifetimebound accessor) still points into -- a silent heap-use-after-
// free. The array subscript defeated two AST-shape checks at once: the old
// const-subversion handler matched only a direct `this->ptr`, and the
// self-referential store check ignored array-element stores. The array peel is
// now centralized (memberThroughArraySubscripts) so `this->arr[i]` is treated as
// the member `this->arr` at every such site; the const-subversion side is now
// loan-based (the mutated receiver's loan roots at a member of the object).
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
class [[gsl::Owner(char)]] MyStr {
  std::string buf{"a long heap string exceeding sso limits.........."};
  std::string *self[1];
public:
  MyStr() { self[0] = &buf; }
  std::string_view view() const [[clang::lifetimebound]] { return buf; }
  void grow() const { self[0]->reserve(1000000); } // const method reallocs buf
  __attribute__((noinline)) char bug() {
    std::string_view v = view(); // v borrows buf's heap data
    grow();                       // reallocs buf -> v dangles
    return v[0];                  // heap-use-after-free
  }
};
int main() {
  MyStr s;
  printf("%c\n", s.bug());
  return 0;
}
