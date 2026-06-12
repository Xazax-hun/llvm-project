// DESC: a borrow stored into a std::string_view field reached through a
// std::unique_ptr member (`box->view = local`) is dropped: the store lands on a
// transient operator-> result origin, not a stable field-of-this origin, so the
// dangling field is missed. The unique_ptr<Inner> member -- an owner of an
// indirection (Inner holds a view) -- is now rejected at the class definition
// (owner-of-indirection on a data member), mirroring the local/element checks.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Inner {
  std::string_view view;
};

struct Outer {
  std::unique_ptr<Inner> inner = std::make_unique<Inner>();
  void bind() {
    std::string local = "a long heap string exceeding the sso buffer size limit!!";
    inner->view = local; // borrow of local stored into the field via unique_ptr->
  }                      // local dies; *inner (and the field) still alive
  char first() const { return inner->view[0]; }
};

int main() {
  Outer o;
  o.bind();
  volatile char c = o.first(); // use-after-free
  return c;
}
