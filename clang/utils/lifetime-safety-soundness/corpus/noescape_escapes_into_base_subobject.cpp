// DESC: a user [[gsl::Owner]] privately inherits std::string_view and its
// constructor stores the (falsely) [[clang::noescape]] argument into that
// std::string_view BASE subobject (`: std::string_view(s)`). The base holds a
// borrow to the caller's local, which dangles once the local dies while the
// heap-allocated owner lives on. The origin model does not represent base
// subobjects, so the store was invisible and the lying noescape silenced the
// only other backstop -- the constructor was modeled as a capture into `this`
// so the noescape verifier flags the escaping parameter. Sibling of
// view_field_via_unique_ptr.cpp (member form) for the base-subobject form.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Owner]] Holder : private std::string_view {
  // LIE: sv escapes into the string_view base subobject and outlives the call.
  Holder(std::string_view sv [[clang::noescape]]) : std::string_view(sv) {}
  char first() const { return this->empty() ? '\0' : this->front(); }
};

int main() {
  Holder *h;
  {
    std::string local = "LOCAL backing string long enough to live on the heap!!";
    h = new Holder(local); // local's buffer borrowed into h's base subobject
  }                        // local dies; h (and its base) dangles
  volatile char c = h->first(); // use of dangling base subobject
  (void)c;
  return 0;
}
