// DESC: two arguments alias because one is a MEMBER of the other. `firstAfterRefill(m,
// m.data)` hands the callee a whole `Model` by const reference and that model's `data`
// vector mutably; the callee borrows `m.data`'s buffer through `m`, then reallocates it
// through the other parameter. No annotation can express that two parameters must not
// alias, which is exactly why the argument-overlap check exists -- and both [[noescape]]
// annotations here are truthful, so nothing else had cause to complain.
//
// The check already reasoned about storage CONTAINMENT rather than mere equality, but only
// downward: a co-argument borrowing AT OR BELOW the mutated storage, which is `f(a, a.b)`
// read as "mutating `a` may reallocate the field `a.b` borrows". Borrowing an object that
// CONTAINS the mutated storage reaches it just as surely and was not tested, so this was
// silent while `f(v, v)` and a view passed alongside its own owner were both caught.
//
// Both directions count now. Disjoint subobjects still do not overlap: neither `m.a` nor
// `m.b` is a prefix of the other.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct Model {
  std::vector<int> data;
};

// Reads an element after topping the container up. Reasonable in isolation: `out` is
// documented as a scratch buffer, `m` as read-only input, and neither escapes.
static int firstAfterRefill(const Model &m [[clang::noescape]],
                            std::vector<int> &out [[clang::noescape]]) {
  auto it = m.data.begin(); // borrow of m.data's buffer, reached through `m`
  out.assign(9000, 7);      // `out` IS m.data -> reallocates, frees the old buffer
  return *it;               // dangling iterator
}

int main() {
  Model m{{1, 2, 3}};
  sink = firstAfterRefill(m, m.data);
  return 0;
}
