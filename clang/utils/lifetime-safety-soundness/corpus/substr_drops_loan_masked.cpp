// DESC: std::string_view::substr is not [[clang::lifetimebound]] (here), so it
// drops the loan -- its result is an untracked borrow. Standalone this is caught
// (lost-loan). The bypass masks it: `v` is first given a valid borrow into a
// long-lived string, then conditionally reassigned the substr result; the
// union of loan sets across the branch stays non-empty (the long-lived borrow),
// hiding that on the substr path `v` lost its borrow. The owner is then
// reallocated and the dangling `v` read. Found by the 4th multi-agent bypass
// hunt (C3). Fixed by marking an untracked borrow-returning call result with an
// Unknown loan that survives dataflow joins.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

static bool always_true() { return true; }

int main() {
  std::string longlived(64, 'L');
  std::string s(64, 'A');
  std::string_view v = longlived; // valid borrow into longlived
  if (always_true())
    v = std::string_view(s).substr(0, 8); // borrow into s, but substr drops it
  for (int i = 0; i < 4000; ++i)
    s.push_back('B'); // reallocates s -> v (pointing into s) dangles
  return v[0];        // use-after-free
}
