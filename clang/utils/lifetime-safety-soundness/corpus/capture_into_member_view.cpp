// DESC: '[[clang::lifetime_capture_by(this)]]' dropped when the receiver is a MEMBER.
// A capture destination has to name the object that will hold the borrow, but the
// capture flowed into the origin of the r-value that reading `h.v` produced -- a
// throwaway the next read of the same member does not share -- so the object never
// received the borrow. It reads as CLEAN rather than as a lost borrow because the
// enclosing object already carries an unrelated loan (`View{keeper}`): the lost-loan
// sentinel fires only when NO borrow information reaches an origin, so a co-resident
// loan hides the drop. Without that prior loan the same code reported only lost-loan,
// never the dangle. Sibling of the derived-to-base receiver case: both are a capture
// landing in a copy of the receiver rather than in the object, one via a conversion
// and one via a member projection.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] View {
  std::string_view sv;
  void set(std::string_view s [[clang::lifetime_capture_by(this)]]) { sv = s; }
};

struct [[gsl::Pointer]] Holder { View v; };

int main() {
  std::string keeper = "a long lived heap string value exceeding the sso buffer!!";
  Holder h{View{keeper}}; // h.v already carries a loan, masking the sentinel
  {
    std::string local = "a long heap string value exceeding the sso buffer now!!";
    h.v.set(local); // captured into a member view
  }
  sink = h.v.sv[0]; // heap-use-after-free
  return 0;
}
