// DESC: a borrow-holding container (std::vector<std::string_view>, an
// owner-of-indirection) is wrapped in a std::pair data member of a user struct.
// std::pair is neither a gsl::Owner nor a gsl::Pointer, and its own
// owner-of-indirection member (pair::first) is declared in a system header, so
// the per-record field-declaration check is suppressed there. The safe model
// now searches the template arguments of an aggregate field type, so the member
// is rejected at the enclosing record's definition.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct Holder {
  std::pair<std::vector<std::string_view>, int> p;
};

volatile char sink;

int main() {
  Holder h;
  {
    std::string s(64, 'q'); // heap-backed owner
    h.p.first.push_back(std::string_view(s));
  }                                  // s destroyed -> the stored view dangles
  sink = h.p.first[0][0];            // heap-use-after-free
  return 0;
}
