// DESC: a static data member of a class TEMPLATE. The sweep over namespace-scope dynamic
// initializers -- the only entry point that reaches this code, since a file-scope VarDecl is
// neither a function scope nor a call-graph node -- enumerated variables by recursing over
// DeclContext::decls() and descending into namespaces, `extern "C"` blocks and records.
//
// An implicitly-instantiated class template specialization is in NONE of those chains: it lives
// in the template's folding set, so no enclosing DeclContext lists it. The dependent PATTERN is
// skipped as well, and correctly so -- `static T t = ...;` says nothing until T is known. The
// two together meant the initializer was analyzed for no instantiation at all: not refused,
// simply never seen. Deleting `template <class T>` made the identical initializer fire, which is
// what localized the gap to the enumeration rather than to the invalidation reasoning.
//
// The same hole covered a nested class inside a class template, a partial specialization, a
// member variable template, and a CRTP base. An out-of-line definition was always reached,
// because that is a namespace-scope declaration in its own right.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

std::string *gp = nullptr;
std::string_view gsv;

// A per-type cache: the initializer borrows from the heap string, then frees it, leaving the
// namespace-scope view dangling. Nothing here is a callable, so only the file-scope-initializer
// sweep could ever have reached it.
template <class T> struct PerType {
  static inline char c =
      (gp = new std::string(72, 'a'), gsv = std::string_view(*gp), delete gp, gsv[0]);
};

int main() {
  sink = PerType<int>::c;
  return 0;
}
