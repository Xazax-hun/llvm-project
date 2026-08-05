// DESC: an imprecise borrow from a [[clang::lifetimebound]] accessor, taken through
// a reference PARAMETER, invalidated by mutating that parameter's owner field.
// Entirely idiomatic, fully sanctioned code: the accessor is const and truthfully
// lifetimebound, the parameter is truthfully [[clang::noescape]], nothing is
// laundered. It was silent because invalidatedObjectRecord could not name the
// object a *parameter* placeholder loan refers to: AccessPath::getAsValueDecl() is
// gated on Kind::ValueDecl and returns null for Kind::PlaceholderParam, whose root
// is a ParmVarDecl. With no record, the conservative "an imprecise borrow into the
// object is invalidated too" arm of the invalidation check was skipped. The
// identical body with a `this` receiver (PlaceholderThis -> getParent()) or a local
// receiver (Kind::ValueDecl) was reported -- the parameter was the only spelling
// that fell through.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Owner]] Doc {
  std::string s;
  // Sanctioned accessor: const, and the returned borrow is declared to be bound
  // to the implicit object. Which subobject it borrows is not expressed, so the
  // borrow is imprecise.
  std::string_view text() const [[clang::lifetimebound]] { return s; }
};

// Sanctioned parameter contract: the borrow must not escape this function.
void render(Doc &d [[clang::noescape]]) {
  std::string_view v = d.text(); // borrows d.s's heap buffer
  d.s.assign(400, 'y');          // reallocates d.s -> v dangles
  sink = v[0];                   // heap-use-after-free
}

int main() {
  Doc d;
  d.s.assign(200, 'x');
  render(d);
  return 0;
}
