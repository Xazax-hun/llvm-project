// DESC: a DEFAULT ARGUMENT that materializes a temporary, bound to a
// [[clang::lifetimebound]] parameter. The default argument's expression belongs to
// the CALLEE's declaration rather than to the caller, so the CFG deliberately does
// not contain it -- adding it would make one Expr appear at every call site
// (PR13385, the FIXME in CFG.cpp). The analysis therefore never sees the temporary:
// no loan is issued for it and no expiry fires, so the borrow it hands over looks
// immortal.
//
// The identical call written explicitly, `h = Holder(std::string(...))`, is reported
// precisely. Modelling the default argument in the generator instead would share one
// expression's origins across every call site -- the aliasing hazard the CFG comment
// is about -- so the call is refused.
//
// (-Wdangling-assignment-gsl, an older and separate Sema check, does report this one;
// it is not part of the lifetime-safety model, and the free-function spelling in the
// lit test escapes it entirely.)
// FLAGS: -Wno-dangling-assignment-gsl
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Pointer(char)]] Holder {
  std::string_view v;
  Holder(std::string_view s [[clang::lifetimebound]] =
             std::string("default value long enough to heap allocate"))
      : v(s) {}
  char first() const { return v[0]; }
};

int flag = 0;

int main() {
  std::string live = "a live string that is long enough to be heap allocated";
  Holder h{live};
  if (!flag)
    h = Holder(); // the default-arg temporary dies at the end of this full-expression
  g_sink = h.first();
  return 0;
}
