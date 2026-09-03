// DESC: a placement new into `this` reconstructs the object holding a borrow of a
// destructor-body local. A placement new constructs into storage that already EXISTS and
// outlives the expression, so a borrow the new object captures comes to rest there -- but the
// initializer flowed only into the new-expression's own pointee origin, a throwaway for a
// placement form, so the object never received it. Three-way control, same class and same
// local, differing only in spelling: `v = t;` caught (dangling-field), `*this = S{t};` caught
// (use-after-scope), `new (this) S{t};` SILENT. Reads clean only once the caller-side
// lost-loan sentinel is masked by giving `s` a valid prior loan -- without that mask the
// sentinel fires (for all three spellings alike), so the mask is what makes this a clean
// bypass rather than a precision gap.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <new>
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] S {
  std::string_view v;
  void go() {
    std::string t("hello world hello world hello world!!!!");
    new (this) S{t};
  }
};

int main() {
  std::string keeper("a long lived heap string exceeding the sso buffer!!");
  S s{keeper};          // pre-existing valid loan: masks the lost-loan sentinel
  s.go();               // reconstructs in place with a dying local
  sink = s.v[0];        // heap-use-after-free
  return 0;
}
