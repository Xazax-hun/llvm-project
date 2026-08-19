// DESC: two indirections to the same storage combined by AGGREGATE INITIALIZATION -- a view
// of `buf` and a mutable pointer to `buf` existing at once, which is an exclusivity
// violation. The question was asked only at CALLS: passing the identical two arguments to a
// free function or to a user-written constructor was reported, while `Session{Token{buf},
// Trailer{&buf}}` was silent, because an aggregate's initializers never reached the check.
//
// The consequence is the sibling-destruction bug this file demonstrates. Members are
// destroyed in reverse declaration order, so ~Trailer appends to the buffer -- reallocating
// it -- and ~Token then reads the view it captured.
//
// The observable failure is order-dependent: declaring Trailer first makes ASan quiet, since
// ~Token then runs before the append. The exclusivity violation is present eitherway, and is
// reported either way -- which is the point of checking the combination rather than the
// destruction order.
//
// What a call and an aggregate differ in is only what each argument BINDS to: a parameter
// versus a field. Everything downstream -- which co-arguments carry an aliasing borrow, the
// pointee chain, the record being mutated -- is the same, and is shared.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char csink;

// Logs the token text when it goes away.
struct [[gsl::Pointer]] Token {
  std::string_view text;
  ~Token() { csink = text[0]; }
};

// Appends a trailer to the buffer when the session ends.
struct [[gsl::Pointer]] Trailer {
  std::string *buf;
  ~Trailer() { buf->append(4000, '!'); }
};

struct [[gsl::Pointer]] Session {
  Token tok;
  Trailer tr;
};

int main() {
  std::string buf(64, 'x');
  Session s{Token{buf}, Trailer{&buf}};
  return 0;
}
