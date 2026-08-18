// DESC: a [[gsl::Pointer]]-like view stored in an UNANNOTATED struct field is
// reached through a unique_ptr (`box->sv = local`); the borrowed local dies
// while the heap-allocated Box (and its view) lives on. The element type Box
// holds a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]],
// so unique_ptr<Box> is an owner-of-indirection: the safe model requires Box to
// be annotated before it can be modeled.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Box {
  std::string_view sv;
};

int main() {
  auto box = std::make_unique<Box>();
  {
    std::string local = "LOCAL backing string long enough to live on the heap!!";
    box->sv = local; // box->sv borrows local
  }                  // local dies; box (and box->sv) still alive
  volatile char c = box->sv.size() ? box->sv[0] : 0; // use of dangling view
  (void)c;
  return 0;
}
