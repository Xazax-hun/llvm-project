// DESC: a [[clang::lifetime_immortal]] function returns a view into a LOCAL
// laundered through std::string_view::substr. substr's result is an untracked
// (Unknown) loan -- not a loan rooted at the local -- so the immortal-violation
// body check used to accept it, while the immortal attribute suppressed the
// caller-side lost-loan. The caller trusts the (false) immortal promise and
// reads the view after the local string is gone.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

[[clang::lifetime_immortal]] std::string_view bad() {
  std::string s = "a long heap string value well beyond the SSO buffer size!!";
  std::string_view v = s; // borrows local s's heap buffer
  return v.substr(0);     // untracked (Unknown) loan; still views the dead local
}

int main() {
  std::string_view v = bad(); // caller trusts the immortal promise
  volatile char c = v.size() ? v[0] : 0; // use-after-free
  (void)c;
  return 0;
}
