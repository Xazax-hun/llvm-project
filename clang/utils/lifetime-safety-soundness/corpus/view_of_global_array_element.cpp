// DESC: a std::string_view borrows an element of a global ARRAY of owners
// (`std::string g_arr[4]`), then the element is reallocated from another
// function. The view-on-mutable-global check keyed on the loan root's own type
// being a gsl::Owner (or a record containing one), but the loan roots at the
// array variable whose type is `std::string[4]` -- isGslOwnerType(array) is
// false and getAsCXXRecordDecl() is null for an array, so the check skipped it.
// Found by the 65th multi-agent bypass hunt (B). Closed by peeling array
// dimensions and testing the element type.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string g_arr[4];

__attribute__((noinline)) std::string_view make() {
  std::string_view v = g_arr[2]; // borrows g_arr[2]'s heap buffer
  return v;
}

__attribute__((noinline)) void mutate() {
  g_arr[2] = std::string(64000, 'B'); // realloc, cross-function
}

int main() {
  g_arr[2] = std::string(70, 'A');
  std::string_view v = make();
  mutate(); // invalidates v
  return v.empty() ? 0 : v[0]; // heap-use-after-free
}
