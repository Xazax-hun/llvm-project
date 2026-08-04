// DESC: `this` passed as an aliasing call argument. A borrow rooted at the
// callee's `$this` placeholder is deliberately not invalidated by a mutation
// reached through a *parameter* -- otherwise every self-mutating method would
// warn. That is sound only because the CALLER-side argument-overlap check flags
// the aliasing call that makes the parameter and `this` the same object. The
// check missed `this`: the `$this` placeholder loan carries neither an issuing
// expression nor a placeholder parameter, so the alias was detected and then
// dropped as unreportable. `report(&r, &r)` warned; `report(this)` did not.
// Now a `$this`-rooted borrow is anchored at the method it belongs to, guarded by
// a containment test so that mutating a *field* (whose loan widens to the same
// `$this` root) does not count as aliasing the whole object.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

volatile char sink;

struct [[gsl::Owner]] Registry {
  std::vector<std::string> items;

  void wipe() { items.clear(); }

  void report(Registry *other [[clang::noescape]]) {
    std::string_view sv = items[0]; // borrow rooted at $this
    other->wipe();                  // frees it, because other == this
    sink = sv[0];                   // heap-use-after-free
  }

  void run() { report(this); } // the aliasing the callee cannot see
};

int main() {
  Registry r;
  r.items.push_back(std::string(100, 'a'));
  r.run();
  return 0;
}
