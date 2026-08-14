// DESC: invalidation laundered through a `void *` parameter. Assumed invalidation asks
// whether a callee can reach a mutable owner through a parameter, and answered it from
// the parameter's pointee type -- which reveals nothing when the pointee is `void`. The
// callee casts it back to the real type and reallocates an owner through it, while the
// signature looks harmless. Note the `[[clang::noescape]]` is TRUTHFUL: the parameter
// really does not escape, so no body verifier applies, and changing the parameter to
// `std::string *` makes it fire immediately.
//
// `void *` was the one launder not already banned: `char *`/`std::byte *` plus a
// reinterpret_cast trips -Wlifetime-safety-type-punning, an inline round-trip in the
// same function trips -Wlifetime-safety-invalidation, a `void *` MEMBER trips
// -Wlifetime-safety-unknown-ownership, and `delete` through a `void *` trips
// -Wlifetime-safety-naked-delete. Only the parameter crossing a call boundary escaped
// -- i.e. the C-interop "opaque userdata" idiom.
//
// The fix is that an opaque pointee cannot be shown to be owner-free, so it must not be
// assumed to be. This costs nothing on ordinary code: the invalidation is only reported
// when a borrow is actually live across the call.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

// Truthful annotation: `p` does not escape. It is still written through.
static void clear_str(void *p [[clang::noescape]]) {
  *static_cast<std::string *>(p) = std::string(); // frees the old buffer
}

int main() {
  std::string s = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  std::string_view v = s; // borrow into s's heap buffer
  clear_str(&s);          // reallocates it through the opaque pointer
  sink = v[0];            // heap-use-after-free
  return 0;
}
