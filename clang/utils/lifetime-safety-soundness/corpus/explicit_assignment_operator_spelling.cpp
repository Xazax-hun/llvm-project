// DESC: a store written as `a.operator=(b)` rather than `a = b`. The operator
// syntax is a CXXOperatorCallExpr and reached the assignment modelling; the
// explicit spelling is an ordinary member call and did not, so it deposited
// NOTHING -- no FieldStore, no DynamicStore. The borrow was lost outright.
//
// Found through an owner whose destructor reads a view member: the store is
// into a member of the object, and the destructor's read of it happens after
// the borrowed string is gone. Only the lost-borrow sentinel remained, and only
// for a NAMED destination -- so the temporary-receiver spelling was completely
// silent. EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Owner(char)]] W {
  // Reads the borrow it holds, after the borrowed-from string is long gone.
  ~W() { g_sink = v[0]; }
  friend int main();

private:
  std::string_view v;
};

int main() {
  W w;
  {
    std::string tmp("a heap string long enough to never be SSO at all here");
    w.v.operator=(tmp); // the explicit spelling of `w.v = tmp`
  }
  return 0; // ~W reads a borrow of the freed string
}
