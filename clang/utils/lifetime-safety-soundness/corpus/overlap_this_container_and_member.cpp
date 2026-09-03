// DESC: the container half of an argument overlap is `this`. `refill(this, data)` hands a
// helper the whole object and one of its member owners mutably; the helper borrows that
// member's buffer through the object pointer and then reallocates it through the other
// parameter. `const Model *self` does not prevent the borrow -- const stops mutation, not
// reading -- so the hazard is the same one `refill(m, m.data)` has.
//
// The overlap check has a separate arm for a borrow of the WHOLE object, because a mutation
// OF that object (or of a base subobject of it) is invisible to a path comparison. That arm
// DECIDED the answer instead of falling through, and it only asks whether the mutated record
// is the object's class or a base of it -- `std::vector<int>` is neither -- so it said no and
// the path comparison that would have said yes never ran. The record test is now one arm
// among several rather than the verdict.
//
// This is the same shape as a lesson already learned one check over, in the invalidation
// containment test: a field-precise special case must not return early when the generic
// comparison is strictly stronger.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct Model {
  std::vector<int> data;

  // Tops the scratch buffer up, then reports an element of the model. Both parameters look
  // independent; the caller is what makes them the same vector.
  static int refill(const Model *self, std::vector<int> &out) {
    const int *p = self->data.data(); // borrow of self->data's buffer, through `self`
    out.assign(9000, 7);              // `out` IS self->data -> frees the old buffer
    return *p;                        // dangling
  }

  int run() { return refill(this, data); }
};

int main() {
  Model m{{1, 2, 3}};
  sink = m.run();
  return 0;
}
