// DESC: storing a borrow THROUGH a [[clang::lifetimebound]] mutable-reference
// accessor of a [[gsl::Owner]]. `b.ref() = std::string_view(s)` assigns a view
// into b's private member through a `std::string_view&`-returning accessor; the
// store lands on the transient call-result origin and is dropped, while both
// backstops are disarmed (gsl::Owner suppresses unknown-ownership; lifetimebound
// suppresses unannotated-indirection on the accessor). The return type
// `std::string_view&` is a reference to a view -- a second level of indirection
// -- now rejected by the single-indirection (multilevel) rule applied to return
// types.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Owner]] Box {
private:
  std::string_view sv;

public:
  Box() = default;
  std::string_view &ref() [[clang::lifetimebound]] { return sv; }
  char read() const { return sv[0]; }
};

volatile char sink;

int main() {
  Box b;
  {
    std::string s = "this is a long string exceeding the SSO buffer length!!";
    b.ref() = std::string_view(s); // store a borrow into b through ref()
  }                                // s destroyed -> b.sv dangles
  sink = b.read();                 // heap-use-after-free
  return 0;
}
