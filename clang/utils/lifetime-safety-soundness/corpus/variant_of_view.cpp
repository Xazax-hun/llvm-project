// DESC: std::variant<int, std::string_view> populated via emplace<string_view>:
// the view is constructed INSIDE emplace from the owner, so no borrow-typed
// value crosses a user call site (dodging the unannotated-indirection store
// backstop that catches a direct `var = sv`), and std::variant is not a
// gsl::Owner so it was not recognized as a container of indirection (unlike
// std::optional<string_view>). std::variant/any keep their value in a union /
// type-erased buffer the origin model does not expand, so the view alternative
// was untracked. It is now flagged as a (pointer-of-)indirection container.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <variant>

static volatile char sink;

int main() {
  std::variant<int, std::string_view> var;
  {
    std::string s = "hello world this is a long string not in the SSO buffer!!";
    var.emplace<std::string_view>(s); // view built inside the variant from owner s
  }                                   // s freed; the view in var dangles
  sink = std::get<std::string_view>(var)[0]; // use-after-free
  return sink;
}
