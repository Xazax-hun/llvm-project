// DESC: a borrow captured into a [[gsl::Owner]]'s private cache via a hidden friend whose
// parameter is named as the capturer. Capturing a borrow into an owner cannot be tracked --
// an owner is meant to own its contents, and a borrow stashed in its opaque members is
// invisible once the owner is passed elsewhere -- and that was already refused for the
// `this` spelling. The identical capture into a NAMED parameter of owner type,
// 'lifetime_capture_by(r)' with 'Record &r', went unrefused. Every annotation here is
// TRUTHFUL: the noescape parameters do not escape, and the friend really does store the
// borrow where it says. Refusing costs nothing the model supports: the attribute's canonical
// documented use captures into a container of views, and such a type is already refused for
// being an owner OF INDIRECTION.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <cstddef>

volatile char sink;

struct [[gsl::Owner]] Record {
  explicit Record(const char *nm [[clang::noescape]]) : name(nm), key(nullptr) {}
  const std::string &owned() const [[clang::lifetimebound]] { return name; }
  char keyChar(std::size_t i) const { return key[i]; }
  friend void setKey(Record &r [[clang::noescape]],
                     std::string_view k [[clang::lifetime_capture_by(r)]]);
private:
  std::string name;   // genuinely owned
  const char *key;    // cached borrow, declared honestly
};

void setKey(Record &r [[clang::noescape]],
            std::string_view k [[clang::lifetime_capture_by(r)]]) { r.key = k.data(); }

int main() {
  Record r("record");
  { std::string key("hello world hello world hello world!!!!"); setKey(r, key); }
  sink = r.keyChar(0);
  return 0;
}
