// DESC: a [[lifetimebound]]-this accessor returns a view into the std::string
// owned by a unique_ptr member; the caller resets the unique_ptr, freeing that
// string while the (object-bound) view is still live. Document does not directly
// own the string, so binding the view to the object is unsound.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Document {
  std::unique_ptr<std::string> content;
  Document() : content(std::make_unique<std::string>(50, 'a')) {}
  std::string_view getView() [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
};

int main() {
  Document doc;
  std::string_view view = doc.getView();
  doc.content.reset();                   // frees *content -> view dangles
  volatile char c = view.size() ? view[0] : 0;
  return c;
}
