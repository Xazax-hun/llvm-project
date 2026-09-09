// DESC: an intrusive reference count's `deref()`, which ends in `delete this`.
// Without an annotation the analysis had no way to know the call destroys the
// object, so a borrow taken into it before the call and used after was missed --
// and the body itself was a FALSE POSITIVE, because the implicit use of `this` at
// function exit read what the `delete this` had just freed.
//
// `__attribute__((ownership_takes(<module>, 1)))` names the implicit object: the
// call deallocates the object it is called on. The call site then behaves exactly
// as an explicit destructor call does, and the body is exempt from the checks that
// assume the object outlives the call. The promise is verified against the body.
// FLAGS: -Wno-lifetime-safety-naked-delete -Wno-lifetime-safety-const-subversion -Wno-lifetime-safety-downcasts
// EXPECT-ASAN: heap-use-after-free
// The ASan compiler is the system clang, which HAS `ownership_takes` but rejects an
// index naming the implicit object, so it cannot compile the annotated form. The
// attribute changes no runtime behaviour, so drop it there and let ASan supply the
// ground truth for the same program. `-Wlifetime-safety-soundness` is the proxy for
// "this is the build under test".
#if __has_warning("-Wlifetime-safety-soundness")
#define TAKES_THIS __attribute__((ownership_takes(RefCounted, 1)))
#else
#define TAKES_THIS
#endif

volatile int g_sink;

class RefCountedBase {
public:
  void ref() const { ++m_refCount; }

protected:
  bool derefBase() const { return !--m_refCount; }

private:
  mutable unsigned m_refCount{1};
};

template <typename T> class RefCounted : public RefCountedBase {
public:
  void deref() const TAKES_THIS {
    if (derefBase())
      delete const_cast<T *>(static_cast<const T *>(this));
  }

protected:
  RefCounted() = default;
  ~RefCounted() = default;
};

class Node : public RefCounted<Node> {
public:
  int m_value{7};
};

int main() {
  Node *n = new Node;
  int *p = &n->m_value; // a borrow INTO the object
  n->deref();           // refcount hits zero: `delete this`
  g_sink = *p;          // heap-use-after-free
  return 0;
}
