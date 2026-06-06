// DESC: an incorrect lifetime annotation hides a dangle - a parameter marked
// [[clang::noescape]] actually escapes via return, so callers trust a wrong
// binding (the result is bound to the un-annotated parameter, not the
// lifetimebound one)
// EXPECT-ASAN: stack-use-after-scope
const int &pick(const int &a [[clang::lifetimebound]],
                const int &b [[clang::noescape]]) {
  return b; // 'b' escapes despite noescape; the real binding is to 'b', not 'a'
}

int main() {
  static int y = 1;
  const int *r = &y;
  {
    int x = 2;
    r = &pick(y, x); // caller trusts the annotation: believes r is bound to 'y'
  }
  return *r; // actually bound to the destroyed 'x'
}
