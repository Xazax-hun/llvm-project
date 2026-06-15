// DESC: a by-reference lambda capture of a view (std::string_view) forms a
// reference to a view -- two levels of indirection. The lambda body reassigns
// the captured view to a shorter-lived borrow; that write-back is not modeled,
// so the view kept its earlier valid loan and lost-loan was masked. The capture
// is now rejected by the single-indirection rule (a by-ref capture of an
// indirection-typed variable).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

int main() {
  std::string longlived = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  std::string_view sv = longlived; // valid loan (alive at the read)
  {
    std::string shortlived = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    auto setit = [&sv, &shortlived]() { sv = shortlived; }; // by-ref capture of a view
    setit();
  }                       // shortlived freed -> sv dangles
  sink = sv.data()[0];    // heap-use-after-free
  return 0;
}
