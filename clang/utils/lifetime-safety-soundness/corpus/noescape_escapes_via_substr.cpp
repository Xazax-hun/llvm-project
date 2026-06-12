// DESC: a [[clang::noescape]] string_view parameter escapes the function via
// `return src.substr(0,5)`. std::string_view::substr is not [[lifetimebound]],
// so its result is an untracked (Unknown) loan rather than a loan rooted at the
// parameter -- the noescape-violation check never attributes the escape to src,
// and (since the result has no local use) nothing else fired either. The caller
// keeps the returned view after the borrowed string is gone.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string_view pick(bool a,
                      std::string_view keep [[clang::lifetimebound]],
                      std::string_view src [[clang::noescape]]) {
  if (a)
    return keep;
  return src.substr(0, 5); // src escapes via an untracked (Unknown) loan
}

int main() {
  std::string longlived = "long lived keep string heap allocated aaaaaaaaaa";
  std::string_view sv;
  {
    std::string shortlived = "short lived src string heap allocated bbbbbbb";
    sv = pick(false, longlived, shortlived); // sv borrows shortlived's buffer
  }                                          // shortlived dies
  volatile char c = sv.size() ? sv[0] : 0;   // use-after-free
  (void)c;
  return 0;
}
