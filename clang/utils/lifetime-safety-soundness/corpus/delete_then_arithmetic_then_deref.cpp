// DESC: `delete` inside a destructor, then pointer arithmetic, then a read. The
// arithmetic was taken to mean "a different element, so bounds, so out of scope"
// -- but after `delete` the whole block is gone, and there is no valid
// neighbouring element to move to. Reached inside a destructor, where freeing a
// member is the normal idiom and the naked-delete refusal is deliberately
// switched off, so nothing else covered it.
//
// The `p + 0` spelling is the same hole without even a displacement: it is
// literally the same address.
// EXPECT-ASAN: heap-use-after-free
volatile int sink;

struct Node {
  int id;
  ~Node() {}
};

class [[gsl::Owner(Node)]] Holder {
  Node *n = new Node{42};

public:
  ~Holder() {
    Node *p = n;
    delete p;     // the storage is freed here
    ++p;          // moves within a block that no longer exists
    sink = p->id; // heap-use-after-free
  }
};

int main() {
  Holder h;
  return 0;
}
