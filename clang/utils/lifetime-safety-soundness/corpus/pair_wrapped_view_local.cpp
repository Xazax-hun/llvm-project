// DESC: the local-variable / return-value form of pair_wrapped_view_member.
// std::pair<std::vector<std::string_view>, int> as a LOCAL (not a record
// member) buried a borrow-holding container. The record-member of-indirection
// check was hardened earlier, but the local/call-result detection paths did not
// search a non-owner aggregate's template arguments, so a view stored into the
// local's vector dangled silently. The local/return/call-result paths now run
// the same nested-template-argument search.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <utility>
#include <vector>

volatile char sink;

int main() {
  std::pair<std::vector<std::string_view>, int> p;
  {
    std::string s(64, 'q'); // heap-backed owner
    p.first.push_back(std::string_view(s));
  }                          // s destroyed -> the stored view dangles
  sink = p.first[0][0];      // heap-use-after-free
  return 0;
}
