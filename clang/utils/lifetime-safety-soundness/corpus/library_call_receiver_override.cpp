// DESC: a user override reached through the IMPLICIT OBJECT ARGUMENT of a library call. A
// library callee is trusted for where it was written; that trust was qualified by asking
// whether the types it is handed are verified -- but a call's arguments do not include its
// receiver, so the receiver was never asked.
//
// A user type deriving from a non-template polymorphic library base reaches the library only
// that way. It appears in no template argument, so the chain that walks those never sees it;
// and calling an inherited member inserts a derived-to-base conversion, so the receiver's
// reported type is `std::pmr::memory_resource` -- library code, which answers "safe". Both
// existing rules therefore looked straight past it, while `memory_resource::allocate`
// dispatched into an unverified override.
//
// Asking the receiver's type AS WRITTEN closes it. The verifier itself was always correct:
// annotating `MyRes` immediately reports the body, which is what localizes this to the
// question never being asked.
//
// Every annotation here is truthful. `lifetime_immortal` on do_allocate is honest -- it
// returns into a function-local static pool -- and the `noescape` parameters do not escape;
// both only silence unrelated diagnostics.
// EXPECT-ASAN: heap-use-after-free
#include <cstddef>
#include <memory_resource>
#include <string>

extern std::string g_victim;

alignas(16) static char g_pool[4096];
static std::size_t g_offset = 0;

struct MyRes : std::pmr::memory_resource {
  [[clang::lifetime_immortal]] void *do_allocate(std::size_t bytes,
                                                 std::size_t align) override {
    // Runs during shutdown, reading a static that is already destroyed.
    volatile char c = g_victim[0];
    (void)c;
    g_offset = (g_offset + align - 1) / align * align;
    void *r = &g_pool[g_offset];
    g_offset += bytes;
    return r;
  }
  void do_deallocate([[clang::noescape]] void *, std::size_t,
                     std::size_t) override {}
  bool do_is_equal([[clang::noescape]] const std::pmr::memory_resource &o)
      const noexcept override {
    return this == &o;
  }
};

struct [[clang::destruction_order_safe]] Client {
  MyRes res;
  ~Client() { (void)res.allocate(8, 8); }
};

Client g_client;                  // constructed first -> destroyed LAST
std::string g_victim(500, 'x');   // constructed second -> destroyed FIRST

int main() { return 0; }
