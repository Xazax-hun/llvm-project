// DESC: an STL API with two levels of indirection is a loan sink. `std::max` on two
// pointers takes and returns `T *const &`, and the soundness net for two levels of
// indirection is the DECLARATION-site refusal -Wlifetime-safety-multilevel-indirection.
// Declaration-site refusals are suppressed in system headers, and since the whole
// standard library is a system header, nothing covered this: write the same signature
// yourself and you get three refusals, put the identical declaration behind
// `#pragma clang system_header` and the translation unit was silent. libc++'s
// `_LIBCPP_LIFETIMEBOUND` on the parameters also keeps
// -Wlifetime-safety-unannotated-indirection quiet.
//
// The generator does emit the sentinel it promises for this family -- the call's result
// carries a loan whose AccessPath is `Unknown`, meaning "this came from a construct I
// could not follow" -- and the InvalidateOrigin fact for `push_back` IS emitted. But
// `AccessPath::isPrefixOf` compares kind and root, so an Unknown path matched NOTHING and
// the invalidation invalidated nothing. The borrow ended up neither tracked nor refused:
// the one state the model is supposed never to be in.
//
// The fix reads the Unknown path as what it says: the mutation may reach any storage, so
// it invalidates every live borrow. The cost is that an unrelated live borrow is also
// reported at such a call -- an Unknown path carries nothing to discriminate on -- and
// nothing at all is reported when no borrow is live.
// EXPECT-ASAN: heap-use-after-free
#include <algorithm>
#include <vector>

volatile int sink;

int main() {
  std::vector<int> v;
  v.reserve(1);
  v.push_back(1);

  std::vector<int> *q = &v;
  int *p = &v[0]; // borrow into v's heap buffer

  // Reaches v through a reference-to-pointer round trip, then reallocates it.
  std::max(q, q)->push_back(99);

  sink = *p; // heap-use-after-free
  return 0;
}
