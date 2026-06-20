// DESC: a [[gsl::Pointer]] view member is bound to a sibling owner member in the
// constructor MEMBER-INITIALIZER list (`S() : buf(...), view(buf) {}`). The
// self-referential check is driven by a FieldStoreFact, which was emitted only
// for an in-body store (`view = buf;`) in handleAssignment, never for the
// member-initializer path -- so the init-list spelling of the identical
// self-referential object was silent. Reallocating `buf` then leaves `view`
// dangling into the freed buffer. Found by the 64th multi-agent bypass hunt (D).
// Closed by emitting a FieldStore for a view/pointer member-initializer too.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct S {
  std::string buf;
  std::string_view view;
  S()
      : buf("long heap-backed content that exceeds the small-string buffer!!"),
        view(buf) {} // view aliases sibling buf
};

__attribute__((noinline)) char bug() {
  S s;
  s.buf = std::string(64000, 'B'); // realloc buf -> s.view dangles into the old buffer
  return s.view.empty() ? '?' : s.view[0]; // heap-use-after-free
}

int main() { return bug(); }
