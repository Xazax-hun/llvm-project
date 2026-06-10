// DESC: a [[lifetimebound]]-this accessor returns a RAW pointer into a member's
// heap buffer; the caller mutates that member, reallocating it and dangling the
// pointer. Like the string_view case, but the borrow is a raw pointer (const
// char* / int*), so the object-bound borrow is not a gsl::Pointer view.
// EXPECT-ASAN: heap-use-after-free
#include <string>

struct Obj {
  std::string buf = std::string(50, 'a');
  const char *data() const [[clang::lifetimebound]] { return buf.c_str(); }
};

int main() {
  Obj o;
  const char *p = o.data(); // p points into o.buf; bound to the whole object
  o.buf.append(10000, 'b'); // reallocates buf -> p dangles
  return p[0];
}
