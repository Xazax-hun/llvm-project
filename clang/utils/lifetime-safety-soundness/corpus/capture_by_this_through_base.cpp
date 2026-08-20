// DESC: '[[clang::lifetime_capture_by(this)]]' dropped through a derived-to-base
// receiver conversion. A capture destination has to name the object that will hold
// the borrow, and an inherited method is called on the derived object through an
// implicit derived-to-base conversion whose own origin is a fresh node
// disconnected from that object -- so the captured borrow flowed into a throwaway
// origin and the object never received it. The identical call on a `Base` object
// WAS reported, so the conversion alone decided whether the capture was modelled.
// The annotation is honest and the callee body is verified; the loss is entirely on
// the caller side, and -Wdangling-capture does not see it either. Sibling of the
// FieldStore emitted a few lines further down, which already peeled this exact
// conversion to key self-referential detection on the derived class's fields.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] Base {
  std::string_view sv;
  void set(std::string_view s [[clang::lifetime_capture_by(this)]]) { sv = s; }
};

struct [[gsl::Pointer]] Derived : Base {};

int main() {
  std::string keep = "kept alive kept alive kept alive keptalive";
  Derived d{{keep}};
  {
    std::string tmp = "temporary temporary temporary temporary!!";
    d.set(tmp); // inherited set: receiver converted to Base&
  }             // tmp dies, but d.sv still views it
  sink = d.sv[0]; // heap-use-after-free
  return 0;
}
