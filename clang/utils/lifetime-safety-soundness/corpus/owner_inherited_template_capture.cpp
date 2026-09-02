// DESC: 'lifetime_capture_by(this)' on an INHERITED member function TEMPLATE of a
// [[gsl::Owner]]. Capturing a borrow into an owner is refused because an owner is meant to
// own its contents and a borrow in its opaque members cannot be tracked; for an inherited
// method that refusal is done at the derived class's completion, and that walk enumerated
// RD->methods(), which does NOT list a FunctionTemplateDecl. So being written as a template
// let the capture through, while the plain spelling right beside it was refused. The cached
// pointer is `protected` rather than public so the owner-public-pointer refusal does not
// cover for it. An owner's OWN member template was already refused, because the attribute
// handler runs per written annotation -- only the inherited path walked methods().
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Base {
protected:
  const char *key = nullptr;   // not public, so owner-public-pointer stays quiet

public:
  char at(int i) const { return key[i]; }
  template <class T>
  void setTmpl(T, std::string_view k [[clang::lifetime_capture_by(this)]]) {
    key = k.data();
  }
};

struct [[gsl::Owner]] Rec : Base { std::string name; };

int main() {
  Rec r;
  { std::string s("hello world hello world hello world!!!!"); r.setTmpl(0, s); }
  sink = r.at(0);
  return 0;
}
