// DESC: a [[clang::noescape]] parameter is forwarded into a callee's
// [[clang::lifetime_capture_by(this)]] parameter, capturing it into the object.
// The noescape promise is a lie -- the borrow escapes into the (caller-scoped)
// object. The capture flows into the whole-object `this` origin and never
// reaches a return/field/global escape point, so the noescape verifier missed
// it. A `this`-capture escape fact now surfaces it. An aggregate-init anchor
// (`H h{longlived}`) seeds h with a valid loan that masks the lost-loan
// backstop, and [[gsl::Pointer]] keeps H tracked (else unknown-ownership fires).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Pointer(char)]] H {
  std::string_view sv;
  void capture(std::string_view in [[clang::lifetime_capture_by(this)]]) {
    sv = in;
  }
  void outer(std::string_view x [[clang::noescape]]) { capture(x); }
};

int main() {
  std::string longlived = "LONGLIVED heap allocated anchor string value here ok";
  H h{longlived}; // anchor: seeds h with a valid loan
  {
    std::string shortlived = "SHORTLIVED heap string freed soon, dangling next";
    h.outer(shortlived); // rebinds h.sv to shortlived's buffer
  }                      // shortlived freed here
  volatile char sink = h.sv[0]; // use-after-free
  return (int)sink;
}
