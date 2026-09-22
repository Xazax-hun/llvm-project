// DESC: a borrow into a container whose ELEMENT TYPE is the container's own record
// -- a tree, a DOM, an AST, a JSON node. An object survives a mutation of its own
// contents, so a holder pointing AT a container is spared while one pointing INTO
// it is reported, and that decision was made by comparing the holder's pointee
// record against the mutated object's record. For `Tree` holding
// `std::vector<Tree>`, a pointer into the element buffer has pointee record `Tree`
// -- the same as a pointer AT the object -- so the type test said "points AT" and
// the reallocation went unreported. No type test can separate the two here.
//
// The loan already distinguishes them: the result of a `[[clang::lifetimebound]]`
// accessor carries an `Interior` (`.*`) path element, meaning "somewhere in there,
// I cannot say where", so its path is `...scene.*` where a pointer AT the object
// has plain `...scene`. Deciding from the loan rather than the type separates them
// -- and does so however the loan is rooted, which matters because the same shape
// written as a method on the container itself roots at `$this` instead of at a
// member seed, and was equally silent.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct Tree {
  int id = 0;
  std::vector<Tree> kids;
  Tree *firstKid() [[clang::lifetimebound]] { return &kids[0]; }
  void addKid() { kids.resize(kids.size() + 1); }
};

struct [[gsl::Pointer]] Editor {
  Tree *scene;
  Tree *selected;
  void edit() {
    selected = scene->firstKid(); // points INTO scene->kids' buffer
    scene->addKid();              // reallocates that buffer
    sink = selected->id;          // heap-use-after-free
  }
};

int main() {
  Tree t;
  t.addKid();
  t.kids[0].id = 7;
  Editor e{&t, nullptr};
  e.edit();
  return 0;
}
