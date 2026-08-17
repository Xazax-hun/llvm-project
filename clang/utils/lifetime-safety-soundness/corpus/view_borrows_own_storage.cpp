// DESC: a [[gsl::Pointer]] view handing out a borrow of its OWN inline storage. For a view
// receiver, a member named `data` / `get` / `c_str` / `begin` / `end` / `cbegin` was treated
// as returning a borrow of what the view points AT rather than of the view object. That holds
// for the standard library -- `string_view::data()` really does return a pointer into the
// viewed buffer -- but it is a guess about the NAME, and a user-defined view can have storage
// of its own.
//
// So the borrow got credited to the viewed object, which outlives the view here, and the use
// after the view died was reported nowhere. Worse, the guess OVERRODE the explicit
// '[[clang::lifetimebound]]' on the accessor, which says the result may refer to *this* object:
// the same struct with the accessor renamed to anything not on the list was caught correctly,
// and adding or removing the annotation made no difference at all.
//
// The fix is that the annotation wins over the guess. It costs nothing for the standard
// library, which annotates none of these accessors, so a borrow from an STL view is still
// attributed to the viewed object. And a user view that really does forward its pointee
// cannot lose tracking by annotating: '[[clang::lifetimebound]]' there is untrue and the body
// verifier reports it as unverifiable.
// EXPECT-ASAN: stack-use-after-scope
#include <string>
#include <string_view>

volatile char sink;

// A view over someone else's characters, plus room for a normalized copy.
struct [[gsl::Pointer(char)]] Normalized {
  std::string_view src;
  char scratch[64];

  // Truthful and verifiable: the result really does point into `*this`.
  const char *data() const [[clang::lifetimebound]] { return scratch; }
};

// optnone so the -O1 build keeps the scope poisoning; the bug is identical at -O0.
__attribute__((optnone)) void run() {
  std::string owner(70, 'k');
  std::string_view text = owner; // a borrow that outlives the view below

  const char *p = nullptr;
  {
    Normalized n{text, {}};
    n.scratch[0] = 'A';
    p = n.data(); // borrows `n`; the name heuristic credited it to `text`
  }               // `n` dies here -- `p` dangles
  sink = p[0];    // stack-use-after-scope
}

int main() {
  run();
  return 0;
}
