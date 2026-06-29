// DESC: a [[gsl::Pointer]] view with a reference-to-pointer member (`const
// std::string *&`, indirection depth 2). A gsl::Pointer record is a tracked leaf
// (its fields are not modeled individually), and the leaf-flow guards drop a
// member whose indirection depth mismatches the view origin -- so the borrow
// read out of the depth-2 member is dropped and a control-flow merge masks the
// lost-loan backstop. The single-indirection ban now applies to record fields
// too, rejecting the depth-2 member at the View definition.
// EXPECT-ASAN: stack-use-after-scope
#include <string>

struct [[gsl::Pointer]] View {
  const std::string *ok;          // depth-1 member (fine)
  const std::string *&danger_ref; // depth-2 reference-to-pointer member
};

volatile char sink;

int main(int argc, char **) {
  std::string safe = "SAFE long heap string aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const std::string *r = nullptr;
  {
    std::string danger = "DANGER long heap string bbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const std::string *pd = &danger;
    View v{&safe, pd};
    if (argc > 100000)
      r = v.ok;         // statically-reachable: valid loan (masks lost-loan)
    else
      r = v.danger_ref; // runtime: dangling borrow read out of the depth-2 member
  }                     // danger expires
  sink = (*r)[0];       // use-after-scope at runtime
  return (int)sink & (int)safe.size() & 0;
}
