// DESC: a [[lifetimebound]]-this accessor returns a view into a member; the
// caller then mutates that member, reallocating it and leaving the view
// dangling. The view is bound to the whole object (the accessor lost which
// subobject it borrows), so the field mutation must invalidate it conservatively.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Document {
  std::string content = std::string(50, 'a');
  std::string_view getView() [[clang::lifetimebound]] {
    return std::string_view(content);
  }
};

int main() {
  Document doc;
  std::string_view view = doc.getView(); // bound to doc per lifetimebound
  doc.content.append(10000, 'b');        // reallocates content -> view dangles
  volatile char c = view.size() ? view[0] : 0;
  return c;
}
