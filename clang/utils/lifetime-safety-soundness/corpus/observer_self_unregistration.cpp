// DESC: observer self-unregistration -- the receiver is a borrow INTO the container
// that is also passed mutably, so the callee's mutation destroys the object whose
// method is running. `items[0]->unregister(items)` borrows `$this.items.*.*` as the
// receiver while passing `$this.items` mutably; `reg.clear()` then destroys *this
// and `this->name` is read afterwards.
//
// The argument-overlap check decided a `$this`-rooted borrow by a coarse
// record-identity test -- does the borrowed object's class equal, or derive from,
// the mutated argument's record. `Registry` is neither the mutated `vector` nor
// derived from it, so it said no, and the path information that shows the alias
// precisely was thrown away. That coarse test is only needed for a borrow of the
// WHOLE object, where every field's loan shares the same `$this` root and a path
// comparison would match them all; a `$this`-rooted path WITH elements names
// specific storage, so the precise comparison decides it -- and a disjoint sibling
// container still diverges. Same lesson as field_argument_reentrancy.cpp, one level
// further out.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <vector>

volatile char sink;

struct Item {
  std::string name;
  void unregister(std::vector<std::unique_ptr<Item>> &reg [[clang::noescape]]) {
    reg.clear();    // destroys *this
    sink = name[0]; // heap-use-after-free on this->name
  }
};

struct Registry {
  std::vector<std::unique_ptr<Item>> items;
  void fire() { items[0]->unregister(items); }
};

int main() {
  Registry r;
  r.items.push_back(std::make_unique<Item>());
  r.items[0]->name = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  r.fire();
  return 0;
}
