// DESC: inheritance launders indirection depth past the refusal. A view out-parameter is refused
// as two levels of indirection, but that measures the STATIC type -- so a `Formatter &` parameter
// whose dynamic type is a [[gsl::Pointer]] measures ONE level and slips past, while the same
// parameter typed as the derived class does not.
//
// The borrow then lands in the caller's object through `lifetime_capture_by(this)` and is
// dropped: `Formatter` has no borrow-holding member, so it has no origin for the borrow to rest
// in, and no later dangling use can be connected to it.
//
// The refusal is on that drop rather than on the depth. `lifetime_capture_by` promises the borrow
// comes to rest in the capturing object; if that object's type has nowhere to put it, the capture
// is unmodelable whatever the depth arithmetic says. Reported at the CALL, which is where the
// borrow is lost, and which also covers a callee only declared in this TU.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
struct Formatter {
  virtual ~Formatter() = default;
  virtual std::string_view format(std::string_view in [[clang::lifetimebound]]
                                  [[clang::lifetime_capture_by(this)]]) { return in; }
};
struct [[gsl::Pointer]] CachingFormatter : Formatter {
  std::string_view cache;
  CachingFormatter(std::string_view init [[clang::lifetimebound]]) : cache(init) {}
  std::string_view format(std::string_view in [[clang::lifetimebound]]
                          [[clang::lifetime_capture_by(this)]]) override {
    cache = in; return in;
  }
};
volatile char g;
void use(std::string_view v [[clang::noescape]]) { for (unsigned i=0;i<v.size();++i) g=v[i]; }
void render(Formatter &f [[clang::noescape]]) {
  std::string tmp = "a heap allocated payload string that is long enough";
  f.format(tmp);            // borrow of tmp lands in the caller's object
}
int main() {
  std::string persistent = "a long lived heap allocated string value";
  CachingFormatter cf(persistent);
  render(cf);
  use(cf.cache);            // dangling
}
