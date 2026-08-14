// DESC: a base destructor reading derived state through a downcast. Inside a destructor of
// `Base`, the derived part of the object has already been destroyed -- bases are destroyed
// after the derived part -- so every heap buffer a derived member owned has been freed. A
// base destructor that reads derived state is therefore a use-after-free, and nothing at
// the read distinguishes it from a live object ([class.cdtor]).
//
// The analysis models `this` as a live COMPLETE object: the downcast, the member access and
// the read are all perfectly modelable operations, so a loan is issued on live-looking
// storage, no Expire fact is generated for the already-destroyed subobject, and no
// conservative backstop fires. There is nothing wrong with any individual step.
//
// Chasing where the conversion happens does not work -- it may be several calls away,
// applied to a parameter or a pointer read from the heap, in a body that lives in another
// translation unit -- so the hazard is DECLARED instead. A function that performs a
// base-to-derived conversion, or calls one marked '[[clang::downcasts]]', must be marked
// itself; a constructor or destructor may not perform one nor call a function marked that
// way. The CRTP "lifecycle hook" is the idiomatic way to write this bug.
//
// 'dynamic_cast' is the escape hatch, and is well defined here: inside a destructor the
// object is treated as being of the destructor's own class, so the cast simply fails.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct Derived;

struct Base {
  ~Base();
};

struct Derived : Base {
  std::string name{"a long enough heap allocated name value here ok............"};
};

// By the time this runs, ~Derived has already freed `name`'s buffer.
Base::~Base() { sink = static_cast<Derived *>(this)->name[0]; }

int main() {
  Derived d;
  sink = d.name[1];
  return 0;
}
