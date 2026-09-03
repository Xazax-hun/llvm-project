// DESC: a truthful-looking [[clang::noescape]] borrow escapes into the caller's object via a
// WHOLE-OBJECT assignment. A store into a named member (`v = q;`) is reported, but writing the
// whole object (`*this = S{q};`) was not: the field-store path only sees a named member, and
// the escape check at function exit inspects the FIELD origins -- while a gsl::Pointer record
// is a single leaf origin with no field edges, so the borrow deposited on the object sits on
// no field's origin. Two functions in the same class, differing only in that: `v = q` caught,
// `*this = S{q}` silent. Same for `S tmp{q}; *this = tmp;` and `new (this) S{q}`. Reads clean
// only once the caller-side lost-loan sentinel is masked by giving the object a valid prior
// loan. The explicit `this->operator=(S{q})` spelling was never clean -- it reports
// assumed-invalidation -- so it is a precision gap rather than a bypass.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] S {
  std::string_view v;
  void A(std::string_view q [[clang::noescape]]) { v = q; }          // caught
  void B(std::string_view q [[clang::noescape]]) { *this = S{q}; }   // was SILENT
};

int main() {
  std::string keeper("a long lived heap string exceeding the sso buffer!!");
  S s{keeper};          // pre-existing valid loan: masks the lost-loan sentinel
  { std::string t("hello world hello world hello world!!!!"); s.B(t); }
  sink = s.v[0];        // heap-use-after-free
  return 0;
}
