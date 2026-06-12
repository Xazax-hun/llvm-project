// DESC: a [[gsl::Owner]] type captures a borrow into a (private) member via
// [[clang::lifetime_capture_by(this)]]; the owner outlives the borrowed-from
// local, so the captured view dangles. The borrow into a private member is
// untracked (members of an owner are opaque). Rather than tracking it, the safe
// programming model bans lifetime_capture_by(this) on owners (use a
// [[gsl::Pointer]] view to hold a borrow), flagging it at the declaration. Found
// by the 4th multi-agent bypass hunt (C2).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

class [[gsl::Owner]] HideOwner {
  std::string_view hidden; // private -> untracked borrow
public:
  HideOwner() = default;
  void stash(std::string_view s [[clang::lifetime_capture_by(this)]]) {
    hidden = s;
  }
  char peek() const { return hidden.front(); }
};

int main() {
  HideOwner h;
  {
    std::string local = "xyzzy-very-long-heap-allocated-string-value-here!!";
    h.stash(local); // h.hidden borrows local's heap buffer
  }                 // local freed; h.hidden dangles
  return (int)h.peek(); // use-after-free
}
