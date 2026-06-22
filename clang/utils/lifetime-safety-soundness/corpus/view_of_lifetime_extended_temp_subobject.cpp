// DESC: a std::string_view borrows an owner SUBOBJECT of a lifetime-extended
// temporary (`const std::string& r = Holder{}.s;`). Binding a reference to a
// subobject of a temporary extends the temporary only to the reference's scope;
// the view copied out (`sv = r`) outlives it. The dual of the round-69 lifetime-
// extension finding: there lost-loan backstopped it, but here string_view is a
// recognized gsl::Pointer so lost-loan was suppressed -- AND VisitMemberExpr
// manufactured a FieldDecl-rooted owner loan for `Holder{}.s` (an xvalue field of
// a storage-less temporary), laundering a lost loan into a tracked one that never
// expires. Found by the multi-agent bypass hunt. Fixed by issuing the owner-field
// loan only when the field access is an lvalue (stable storage); a field of a
// temporary stays lost -> -Wlifetime-safety-lost-loan.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Holder {
  std::string s = std::string(2000, 'A');
};

volatile char sink;
int main() {
  std::string_view sv;
  {
    const std::string &r = Holder{}.s; // extends Holder temp to r's (inner) scope
    sv = r;                            // sv borrows the extended subobject
  }                                    // r's scope ends -> Holder temp destroyed
  std::string churn(2100, 'Z');        // reuse the freed heap region
  sink = sv[1000];                     // use-after-free
  return 0;
}
