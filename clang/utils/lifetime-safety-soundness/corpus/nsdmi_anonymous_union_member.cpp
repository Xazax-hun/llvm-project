// DESC: a hazard in a default member initializer belonging only to an ANONYMOUS UNION member.
// A synthesized constructor is analyzed only when the class has something for it to apply, and
// that gate asked the class's FIELD list. A class's field list holds one unnamed field for an
// anonymous union or struct, and the initializer belongs to a member inside it -- so a class
// whose only initializer is on such a member looked like a class with none, and the analysis
// was skipped entirely.
//
// The controls pin it exactly: removing the union reports, `S s{}` (aggregate initialization
// rather than the implicit constructor) reports, and adding ANY named field -- even
// `int n = 0;` -- reports. An anonymous struct behaves the same as an anonymous union.
//
// Nothing here reads the union member, which is what keeps the file clean: reading one is
// separately refused, and a probe that reads it draws that refusal instead and hides the
// question.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string *gp;
std::string_view gsv;
volatile int sink;

struct S {
  union {
    int a = (gp = new std::string("a long heap-allocated string value"),
             gsv = std::string_view(*gp), // borrow the heap buffer
             delete gp,                   // free it
             (int)gsv[0]);                // read through the dangling view
    long b;
  };
};

int main() {
  S s;
  sink = (int)(long)&s;
  return 0;
}
