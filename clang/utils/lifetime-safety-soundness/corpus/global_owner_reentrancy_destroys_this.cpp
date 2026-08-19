// DESC: reentrancy that destroys `*this`. A global scene owns the nodes; a node's
// method asks the scene to clear itself, then reads its own member. The mutation
// of the scene IS recognized (an InvalidateOrigin on the global), but the borrow
// read afterwards is rooted at the `$this` placeholder -- a caller-scope root the
// intra-procedural analysis never expires -- and no edge says the scene owns
// `*this`, so the invalidation never reached it. The model already demands an
// annotation on every reference PARAMETER that can hold a borrow (which catches
// the same shape written with the scene as a parameter, and the back-pointer
// spelling), but `this` had no equivalent demand and `global.method()` was exempt
// from the mutable-global rule -- so the bug sat exactly where the two exemptions
// met. A non-const method call on a mutable-owner global is now reported: it is a
// mutable borrow of that global with all the reach the callee's body has.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <vector>

volatile char sink;

struct Node {
  std::string label = "a long heap allocated label value ok!!!";
  void update();
};

struct Scene {
  std::vector<std::unique_ptr<Node>> objs;
  void killAll() { objs.clear(); } // destroys every Node
  void tick() { objs[0]->update(); }
  void add() { objs.push_back(std::make_unique<Node>()); }
};

Scene g_scene;

void Node::update() {
  g_scene.killAll(); // *this is destroyed here
  sink = label[0];   // heap-use-after-free
}

int main() {
  g_scene.add();
  g_scene.tick();
  return 0;
}
